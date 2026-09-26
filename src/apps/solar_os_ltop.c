#include "solar_os_ltop.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if __has_include("freertos/freertos_debug.h")
#include "freertos/freertos_debug.h"
#else
#include "esp_private/freertos_debug.h"
#endif
#include "solar_os_keys.h"
#include "solar_os_memory.h"
#include "solar_os_terminal.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define LTOP_SAMPLE_INTERVAL_MS 1000U
#define LTOP_TICK_DEADLINE_MS 50U
#define LTOP_TASK_HEADROOM 8U
#define LTOP_TASK_CAPACITY_MIN 32U
#define LTOP_TASK_CAPACITY_MAX 256U

typedef struct {
    TaskHandle_t handle;
    UBaseType_t number;
    configRUN_TIME_COUNTER_TYPE runtime;
} ltop_previous_task_t;

typedef struct {
    char name[configMAX_TASK_NAME_LEN];
    eTaskState state;
    UBaseType_t priority;
    uint64_t runtime_delta;
    uint64_t stack_used_bytes;
    uint16_t cpu_tenths;
    bool stack_used_known;
} ltop_task_row_t;

typedef struct {
    size_t task_width;
    size_t state_col;
    size_t priority_col;
    size_t cpu_col;
    size_t stack_col;
    size_t stack_width;
    bool show_priority;
} ltop_table_layout_t;

typedef struct {
    solar_os_tui_t tui;
    TaskStatus_t *snapshot;
    TaskSnapshot_t *stack_snapshots;
    ltop_previous_task_t *previous;
    ltop_task_row_t *tasks;
    size_t capacity;
    size_t previous_count;
    size_t task_count;
    size_t snapshot_count;
    size_t top;
    configRUN_TIME_COUNTER_TYPE previous_total;
    uint16_t core_load_tenths[configNUMBER_OF_CORES];
    bool core_load_known[configNUMBER_OF_CORES];
    bool baseline_ready;
    bool sample_ready;
    esp_err_t sample_error;
} ltop_state_t;

static void *ltop_state;
#define ltop (*(ltop_state_t *)ltop_state)

static void ltop_free_buffers(void)
{
    solar_os_memory_free(ltop.snapshot);
    solar_os_memory_free(ltop.stack_snapshots);
    solar_os_memory_free(ltop.previous);
    solar_os_memory_free(ltop.tasks);
    ltop.snapshot = NULL;
    ltop.stack_snapshots = NULL;
    ltop.previous = NULL;
    ltop.tasks = NULL;
    ltop.capacity = 0U;
    ltop.previous_count = 0U;
    ltop.task_count = 0U;
    ltop.snapshot_count = 0U;
}

static esp_err_t ltop_ensure_capacity(size_t needed)
{
    if (needed <= ltop.capacity) {
        return ESP_OK;
    }
    if (needed > LTOP_TASK_CAPACITY_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t capacity = ltop.capacity > 0U ?
        ltop.capacity : LTOP_TASK_CAPACITY_MIN;
    while (capacity < needed && capacity < LTOP_TASK_CAPACITY_MAX) {
        capacity *= 2U;
    }
    if (capacity > LTOP_TASK_CAPACITY_MAX) {
        capacity = LTOP_TASK_CAPACITY_MAX;
    }

    TaskStatus_t *snapshot = solar_os_memory_calloc(
        capacity,
        sizeof(*snapshot),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "ltop.snapshot");
    TaskSnapshot_t *stack_snapshots = solar_os_memory_calloc(
        capacity,
        sizeof(*stack_snapshots),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "ltop.stacks");
    ltop_previous_task_t *previous = solar_os_memory_calloc(
        capacity,
        sizeof(*previous),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "ltop.previous");
    ltop_task_row_t *tasks = solar_os_memory_calloc(
        capacity,
        sizeof(*tasks),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "ltop.tasks");
    if (snapshot == NULL || stack_snapshots == NULL ||
        previous == NULL || tasks == NULL) {
        solar_os_memory_free(snapshot);
        solar_os_memory_free(stack_snapshots);
        solar_os_memory_free(previous);
        solar_os_memory_free(tasks);
        return ESP_ERR_NO_MEM;
    }

    ltop_free_buffers();
    ltop.snapshot = snapshot;
    ltop.stack_snapshots = stack_snapshots;
    ltop.previous = previous;
    ltop.tasks = tasks;
    ltop.capacity = capacity;
    ltop.baseline_ready = false;
    ltop.sample_ready = false;
    return ESP_OK;
}

static uint64_t ltop_counter_delta(configRUN_TIME_COUNTER_TYPE current,
                                   configRUN_TIME_COUNTER_TYPE previous)
{
    return (uint64_t)((configRUN_TIME_COUNTER_TYPE)(current - previous));
}

static const ltop_previous_task_t *ltop_find_previous(
    const TaskStatus_t *task)
{
    for (size_t index = 0U; index < ltop.previous_count; index++) {
        if (ltop.previous[index].handle == task->xHandle &&
            ltop.previous[index].number == task->xTaskNumber) {
            return &ltop.previous[index];
        }
    }
    return NULL;
}

static const TaskSnapshot_t *ltop_find_stack_snapshot(
    TaskHandle_t handle,
    UBaseType_t snapshot_count)
{
    for (UBaseType_t index = 0U; index < snapshot_count; index++) {
        if (ltop.stack_snapshots[index].pxTCB == (void *)handle) {
            return &ltop.stack_snapshots[index];
        }
    }
    return NULL;
}

static uint64_t ltop_stack_used_bytes(
    const TaskStatus_t *task,
    const TaskSnapshot_t *stack_snapshot,
    bool *known)
{
    *known = false;
    if (task->pxStackBase == NULL || stack_snapshot == NULL ||
        stack_snapshot->pxEndOfStack == NULL) {
        return 0U;
    }

    const uintptr_t base = (uintptr_t)task->pxStackBase;
    const uintptr_t end = (uintptr_t)stack_snapshot->pxEndOfStack;
    const uint64_t stack_bytes = (uint64_t)(base <= end ?
        end - base : base - end) + sizeof(StackType_t);
    const uint64_t minimum_free =
        (uint64_t)task->usStackHighWaterMark * sizeof(StackType_t);
    *known = minimum_free <= stack_bytes;
    return *known ? stack_bytes - minimum_free : 0U;
}

static int ltop_idle_core(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    if (configNUMBER_OF_CORES == 1 && strcmp(name, "IDLE") == 0) {
        return 0;
    }
    for (int core = 0; core < configNUMBER_OF_CORES; core++) {
        char expected[12];
        snprintf(expected, sizeof(expected), "IDLE%d", core);
        if (strcmp(name, expected) == 0) {
            return core;
        }
    }
    return -1;
}

static uint16_t ltop_cpu_tenths(uint64_t runtime_delta,
                                uint64_t total_delta)
{
    if (total_delta == 0U) {
        return 0U;
    }
    uint64_t tenths = (runtime_delta * 1000ULL + total_delta / 2ULL) /
        total_delta;
    if (tenths > 1000ULL) {
        tenths = 1000ULL;
    }
    return (uint16_t)tenths;
}

static void ltop_save_baseline(configRUN_TIME_COUNTER_TYPE total_runtime,
                               size_t task_count)
{
    for (size_t index = 0U; index < task_count; index++) {
        ltop.previous[index] = (ltop_previous_task_t){
            .handle = ltop.snapshot[index].xHandle,
            .number = ltop.snapshot[index].xTaskNumber,
            .runtime = ltop.snapshot[index].ulRunTimeCounter,
        };
    }
    ltop.previous_count = task_count;
    ltop.previous_total = total_runtime;
    ltop.baseline_ready = true;
}

static int ltop_compare_tasks(const void *left_value,
                              const void *right_value)
{
    const ltop_task_row_t *left = left_value;
    const ltop_task_row_t *right = right_value;
    if (left->runtime_delta < right->runtime_delta) {
        return 1;
    }
    if (left->runtime_delta > right->runtime_delta) {
        return -1;
    }
    return strcasecmp(left->name, right->name);
}

static esp_err_t ltop_snapshot_tasks(void)
{
#if (configUSE_TRACE_FACILITY == 1) && (configGENERATE_RUN_TIME_STATS == 1)
    size_t required = (size_t)uxTaskGetNumberOfTasks() + LTOP_TASK_HEADROOM;
    esp_err_t error = ltop_ensure_capacity(required);
    if (error != ESP_OK) {
        return error;
    }

    configRUN_TIME_COUNTER_TYPE total_runtime = 0U;
    UBaseType_t count = uxTaskGetSystemState(ltop.snapshot,
                                              (UBaseType_t)ltop.capacity,
                                              &total_runtime);
    if (count == 0U) {
        required = (size_t)uxTaskGetNumberOfTasks() + LTOP_TASK_HEADROOM;
        error = ltop_ensure_capacity(required);
        if (error != ESP_OK) {
            return error;
        }
        count = uxTaskGetSystemState(ltop.snapshot,
                                     (UBaseType_t)ltop.capacity,
                                     &total_runtime);
    }
    if (count == 0U) {
        return ESP_FAIL;
    }

    ltop.snapshot_count = count;
    if (!ltop.baseline_ready) {
        ltop_save_baseline(total_runtime, count);
        ltop.sample_ready = false;
        return ESP_OK;
    }

    const uint64_t total_delta =
        ltop_counter_delta(total_runtime, ltop.previous_total);
    if (total_delta == 0U) {
        ltop_save_baseline(total_runtime, count);
        ltop.sample_ready = false;
        return ESP_OK;
    }

    vTaskSuspendAll();
    const UBaseType_t stack_snapshot_count = uxTaskGetSnapshotAll(
        ltop.stack_snapshots,
        (UBaseType_t)ltop.capacity,
        NULL);
    (void)xTaskResumeAll();

    memset(ltop.core_load_known, 0, sizeof(ltop.core_load_known));
    memset(ltop.core_load_tenths, 0, sizeof(ltop.core_load_tenths));
    ltop.task_count = 0U;
    for (UBaseType_t index = 0U; index < count; index++) {
        const TaskStatus_t *task = &ltop.snapshot[index];
        const ltop_previous_task_t *previous = ltop_find_previous(task);
        const uint64_t runtime_delta = previous != NULL ?
            ltop_counter_delta(task->ulRunTimeCounter, previous->runtime) : 0U;
        const int idle_core = ltop_idle_core(task->pcTaskName);
        if (idle_core >= 0) {
            const uint16_t idle_tenths = ltop_cpu_tenths(runtime_delta,
                                                         total_delta);
            ltop.core_load_tenths[idle_core] =
                (uint16_t)(1000U - idle_tenths);
            ltop.core_load_known[idle_core] = previous != NULL;
            continue;
        }

        ltop_task_row_t *row = &ltop.tasks[ltop.task_count++];
        strlcpy(row->name,
                task->pcTaskName != NULL ? task->pcTaskName : "?",
                sizeof(row->name));
        row->state = task->eCurrentState;
        row->priority = task->uxCurrentPriority;
        row->runtime_delta = runtime_delta;
        row->stack_used_bytes = ltop_stack_used_bytes(
            task,
            ltop_find_stack_snapshot(task->xHandle, stack_snapshot_count),
            &row->stack_used_known);
        row->cpu_tenths = previous != NULL ?
            ltop_cpu_tenths(runtime_delta, total_delta) : 0U;
    }

    ltop_save_baseline(total_runtime, count);
    qsort(ltop.tasks,
          ltop.task_count,
          sizeof(*ltop.tasks),
          ltop_compare_tasks);
    ltop.sample_ready = true;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static char ltop_task_state_char(eTaskState state)
{
    switch (state) {
    case eRunning:
        return 'R';
    case eReady:
        return 'r';
    case eBlocked:
        return 'B';
    case eSuspended:
        return 'S';
    case eDeleted:
        return 'D';
    case eInvalid:
    default:
        return '?';
    }
}

static void ltop_format_bytes(uint64_t bytes,
                              char *buffer,
                              size_t buffer_len)
{
    if (bytes < 1024ULL) {
        snprintf(buffer, buffer_len, "%" PRIu64 " B", bytes);
    } else if (bytes < 1024ULL * 1024ULL) {
        const uint64_t tenths = (bytes * 10ULL + 512ULL) / 1024ULL;
        snprintf(buffer,
                 buffer_len,
                 "%" PRIu64 ".%" PRIu64 " KiB",
                 tenths / 10ULL,
                 tenths % 10ULL);
    } else {
        const uint64_t tenths =
            (bytes * 10ULL + 512ULL * 1024ULL) / (1024ULL * 1024ULL);
        snprintf(buffer,
                 buffer_len,
                 "%" PRIu64 ".%" PRIu64 " MiB",
                 tenths / 10ULL,
                 tenths % 10ULL);
    }
}

static ltop_table_layout_t ltop_table_layout(size_t cols)
{
    const size_t gap = cols >= 48U ? 2U : 1U;
    const bool show_priority = cols >= 32U;
    const size_t stack_width = cols >= 48U ? 10U : 9U;
    const size_t fixed_width = 1U + 5U + stack_width +
        (show_priority ? 3U : 0U) + gap * (show_priority ? 4U : 3U);
    ltop_table_layout_t layout = {
        .task_width = cols > fixed_width ? cols - fixed_width : 1U,
        .stack_width = stack_width,
        .show_priority = show_priority,
    };
    layout.state_col = layout.task_width + gap;
    if (show_priority) {
        layout.priority_col = layout.state_col + 1U + gap;
        layout.cpu_col = layout.priority_col + 3U + gap;
    } else {
        layout.cpu_col = layout.state_col + 1U + gap;
    }
    layout.stack_col = layout.cpu_col + 5U + gap;
    return layout;
}

static void ltop_write_right_aligned(size_t row,
                                     size_t col,
                                     size_t width,
                                     const char *text,
                                     uint8_t attr)
{
    char cell[16];
    snprintf(cell, sizeof(cell), "%*s", (int)width, text);
    (void)solar_os_tui_write_cell(&ltop.tui,
                                  row,
                                  col,
                                  width,
                                  cell,
                                  attr);
}

static void ltop_render_table_header(size_t row,
                                     size_t cols,
                                     const ltop_table_layout_t *layout)
{
    (void)solar_os_tui_fill(&ltop.tui,
                            row,
                            0U,
                            1U,
                            cols,
                            ' ',
                            SOLAR_OS_TUI_ATTR_INVERSE);
    (void)solar_os_tui_write_cell(&ltop.tui,
                                  row,
                                  0U,
                                  layout->task_width,
                                  "TASK",
                                  SOLAR_OS_TUI_ATTR_INVERSE);
    (void)solar_os_tui_write_cell(&ltop.tui,
                                  row,
                                  layout->state_col,
                                  1U,
                                  "S",
                                  SOLAR_OS_TUI_ATTR_INVERSE);
    if (layout->show_priority) {
        ltop_write_right_aligned(row,
                                 layout->priority_col,
                                 3U,
                                 "PRI",
                                 SOLAR_OS_TUI_ATTR_INVERSE);
    }
    ltop_write_right_aligned(row,
                             layout->cpu_col,
                             5U,
                             "CPU%",
                             SOLAR_OS_TUI_ATTR_INVERSE);
    ltop_write_right_aligned(row,
                             layout->stack_col,
                             layout->stack_width,
                             "STACK",
                             SOLAR_OS_TUI_ATTR_INVERSE);
}

static void ltop_render_task_row(size_t row,
                                 size_t cols,
                                 const ltop_table_layout_t *layout,
                                 const ltop_task_row_t *task)
{
    (void)solar_os_tui_write_cell(&ltop.tui,
                                  row,
                                  0U,
                                  cols,
                                  "",
                                  SOLAR_OS_TUI_ATTR_NORMAL);
    if (task == NULL) {
        return;
    }

    (void)solar_os_tui_write_cell(&ltop.tui,
                                  row,
                                  0U,
                                  layout->task_width,
                                  task->name,
                                  SOLAR_OS_TUI_ATTR_NORMAL);
    char state[2] = {ltop_task_state_char(task->state), '\0'};
    (void)solar_os_tui_write_cell(&ltop.tui,
                                  row,
                                  layout->state_col,
                                  1U,
                                  state,
                                  SOLAR_OS_TUI_ATTR_NORMAL);
    if (layout->show_priority) {
        char priority[8];
        snprintf(priority, sizeof(priority), "%u", (unsigned)task->priority);
        ltop_write_right_aligned(row,
                                 layout->priority_col,
                                 3U,
                                 priority,
                                 SOLAR_OS_TUI_ATTR_NORMAL);
    }
    char cpu[8];
    snprintf(cpu,
             sizeof(cpu),
             "%u.%u",
             (unsigned)(task->cpu_tenths / 10U),
             (unsigned)(task->cpu_tenths % 10U));
    ltop_write_right_aligned(row,
                             layout->cpu_col,
                             5U,
                             cpu,
                             SOLAR_OS_TUI_ATTR_NORMAL);
    char stack[16];
    if (task->stack_used_known) {
        ltop_format_bytes(task->stack_used_bytes, stack, sizeof(stack));
    } else {
        strlcpy(stack, "-", sizeof(stack));
    }
    ltop_write_right_aligned(row,
                             layout->stack_col,
                             layout->stack_width,
                             stack,
                             SOLAR_OS_TUI_ATTR_NORMAL);
}

static size_t ltop_table_first_row(void)
{
    return 4U + (size_t)configNUMBER_OF_CORES;
}

static size_t ltop_visible_rows(void)
{
    const size_t rows = solar_os_tui_rows(&ltop.tui);
    const size_t bottom = solar_os_tui_screen_bottom_rows(&ltop.tui, 1U);
    const size_t first = ltop_table_first_row();
    return rows > first + bottom ? rows - first - bottom : 0U;
}

static void ltop_reconcile_scroll(void)
{
    const size_t visible = ltop_visible_rows();
    const size_t max_top = ltop.task_count > visible ?
        ltop.task_count - visible : 0U;
    if (ltop.top > max_top) {
        ltop.top = max_top;
    }
}

static void ltop_render_memory_bar(
    size_t row,
    size_t cols,
    const char *label,
    const solar_os_memory_region_status_t *region)
{
    const bool available = region->total > 0U;
    const uint64_t used = available && region->total >= region->free ?
        (uint64_t)(region->total - region->free) : 0U;
    (void)solar_os_tui_progress_bar(&ltop.tui,
                                    row,
                                    0U,
                                    cols,
                                    label,
                                    used,
                                    region->total,
                                    available);
}

static void ltop_render(void)
{
    const size_t rows = solar_os_tui_rows(&ltop.tui);
    const size_t cols = solar_os_tui_cols(&ltop.tui);
    if (cols < 24U ||
        rows < (size_t)configNUMBER_OF_CORES + 6U) {
        solar_os_tui_draw_too_small(&ltop.tui, "ltop");
        solar_os_tui_refresh(&ltop.tui);
        return;
    }

    char detail[64];
    if (ltop.sample_error != ESP_OK) {
        snprintf(detail,
                 sizeof(detail),
                 "sample error: %s",
                 esp_err_to_name(ltop.sample_error));
    } else if (!ltop.sample_ready) {
        strlcpy(detail, "collecting 1 s sample", sizeof(detail));
    } else {
        uint32_t load_tenths = 0U;
        bool load_known = true;
        for (int core = 0; core < configNUMBER_OF_CORES; core++) {
            load_tenths += ltop.core_load_tenths[core];
            load_known = load_known && ltop.core_load_known[core];
        }
        if (load_known) {
            snprintf(detail,
                     sizeof(detail),
                     "%u tasks | CPU %u.%u%%",
                     (unsigned)ltop.snapshot_count,
                     (unsigned)(load_tenths / 10U),
                     (unsigned)(load_tenths % 10U));
        } else {
            snprintf(detail,
                     sizeof(detail),
                     "%u tasks | CPU ...",
                     (unsigned)ltop.snapshot_count);
        }
    }
    solar_os_tui_draw_title(&ltop.tui, "ltop", detail);

    for (int core = 0; core < configNUMBER_OF_CORES; core++) {
        char label[8];
        snprintf(label, sizeof(label), "CPU%d", core);
        (void)solar_os_tui_progress_bar(
            &ltop.tui,
            1U + (size_t)core,
            0U,
            cols,
            label,
            ltop.core_load_tenths[core],
            1000U,
            ltop.sample_ready && ltop.core_load_known[core]);
    }

    solar_os_memory_status_t memory;
    solar_os_memory_get_status(&memory);
    const size_t memory_row = 1U + (size_t)configNUMBER_OF_CORES;
    ltop_render_memory_bar(memory_row,
                           cols,
                           "IRAM",
                           &memory.internal);
    ltop_render_memory_bar(memory_row + 1U,
                           cols,
                           "ERAM",
                           &memory.external);

    const size_t header_row = memory_row + 2U;
    const ltop_table_layout_t layout = ltop_table_layout(cols);
    ltop_render_table_header(header_row, cols, &layout);

    ltop_reconcile_scroll();
    const size_t visible = ltop_visible_rows();
    for (size_t row_index = 0U; row_index < visible; row_index++) {
        const size_t task_index = ltop.top + row_index;
        const size_t screen_row = ltop_table_first_row() + row_index;
        if (!ltop.sample_ready && row_index == 0U) {
            (void)solar_os_tui_write_cell(&ltop.tui,
                                          screen_row,
                                          0U,
                                          cols,
                                          "Collecting interval data...",
                                          SOLAR_OS_TUI_ATTR_NORMAL);
            continue;
        }
        ltop_render_task_row(screen_row,
                             cols,
                             &layout,
                             task_index < ltop.task_count ?
                                &ltop.tasks[task_index] : NULL);
    }

    if (!solar_os_tui_screen_fullscreen(&ltop.tui)) {
        solar_os_tui_draw_help(&ltop.tui,
                               "Up/Down scroll  r reset  q quit");
    }
    solar_os_tui_set_cursor_visible(&ltop.tui, false);
    solar_os_tui_refresh(&ltop.tui);
}

static void ltop_reset_sample(void)
{
    ltop.baseline_ready = false;
    ltop.sample_ready = false;
    ltop.sample_error = ltop_snapshot_tasks();
}

static esp_err_t ltop_start(solar_os_context_t *ctx)
{
    memset(&ltop, 0, sizeof(ltop));
    esp_err_t error = solar_os_tui_screen_begin(&ltop.tui, ctx);
    if (error != ESP_OK) {
        return error;
    }
    ltop_reset_sample();
    if (ltop.sample_error != ESP_OK) {
        error = ltop.sample_error;
        solar_os_tui_end(&ltop.tui);
        ltop_free_buffers();
        memset(&ltop, 0, sizeof(ltop));
        return error;
    }
    ltop_render();
    return ESP_OK;
}

static void ltop_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&ltop.tui, true);
    solar_os_tui_refresh(&ltop.tui);
    solar_os_tui_end(&ltop.tui);
    ltop_free_buffers();
    memset(&ltop, 0, sizeof(ltop));
}

static void ltop_resume(solar_os_context_t *ctx)
{
    (void)ctx;
    ltop_reset_sample();
    ltop_render();
}

static void ltop_title(solar_os_context_t *ctx,
                       char *buffer,
                       size_t buffer_len)
{
    (void)ctx;
    if (buffer != NULL && buffer_len > 0U) {
        strlcpy(buffer, "ltop", buffer_len);
    }
}

static bool ltop_event(solar_os_context_t *ctx,
                       const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        ltop.sample_error = ltop_snapshot_tasks();
        ltop_render();
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        ltop_resume(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t key = (uint8_t)event->data.ch;
    if (key == SOLAR_OS_KEY_APP_EXIT ||
        key == SOLAR_OS_KEY_ESCAPE ||
        key == 'q' ||
        key == 'Q') {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }

    const size_t visible = ltop_visible_rows();
    switch (key) {
    case SOLAR_OS_KEY_UP:
    case 'k':
        if (ltop.top > 0U) {
            ltop.top--;
        }
        break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        if (ltop.top + visible < ltop.task_count) {
            ltop.top++;
        }
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        ltop.top = ltop.top > visible ? ltop.top - visible : 0U;
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        if (ltop.top + visible < ltop.task_count) {
            ltop.top += visible;
        }
        break;
    case SOLAR_OS_KEY_HOME:
    case 'g':
        ltop.top = 0U;
        break;
    case SOLAR_OS_KEY_END:
    case 'G':
        ltop.top = ltop.task_count;
        break;
    case 'r':
    case 'R':
        ltop_reset_sample();
        break;
    default:
        return true;
    }
    ltop_reconcile_scroll();
    ltop_render();
    return true;
}

const solar_os_app_t solar_os_ltop_app = {
    .name = "ltop",
    .summary = "live task and per-core CPU monitor",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = ltop_start,
    .resume = ltop_resume,
    .stop = ltop_stop,
    .event = ltop_event,
    .title = ltop_title,
    .state_slot = &ltop_state,
    .state_size = sizeof(ltop_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = LTOP_SAMPLE_INTERVAL_MS,
    .tick_deadline_ms = LTOP_TICK_DEADLINE_MS,
};
