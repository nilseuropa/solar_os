#include "solar_os_graffiti_job.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "solar_os_display.h"
#include "solar_os_graffiti.h"
#include "solar_os_input.h"
#include "solar_os_jobs.h"
#include "solar_os_memory.h"
#include "solar_os_queue.h"
#include "solar_os_shell_io.h"
#include "solar_os_task.h"
#include "solar_os_terminal.h"

#define GRAFFITI_STROKE_POINT_MAX 128U
#define GRAFFITI_QUEUE_DEPTH 2U
#define GRAFFITI_WORKER_STACK 6144U
#define GRAFFITI_WORKER_PRIORITY (tskIDLE_PRIORITY + 1)

typedef struct {
    solar_os_graffiti_zone_t zone;
    uint16_t count;
    solar_os_unistroke_point_t points[GRAFFITI_STROKE_POINT_MAX];
} graffiti_stroke_t;

typedef struct {
    bool running;
    bool tracking;
    uint8_t pointer_id;
    uint16_t display_width;
    graffiti_stroke_t active;
    void *recognizer;
    QueueHandle_t queue;
    TaskHandle_t worker_task;
    volatile bool worker_done;
    solar_os_input_source_t keyboard_source;
    solar_os_graffiti_case_state_t case_state;
    uint32_t recognized;
    uint32_t rejected;
    uint32_t dropped;
} graffiti_job_state_t;

SOLAR_OS_APP_STATIC_SRAM_EXCEPTION(
    "pointer filter and worker share one job lifecycle state")
static graffiti_job_state_t graffiti_job;

static void graffiti_job_add_point(graffiti_stroke_t *stroke, int16_t x, int16_t y)
{
    if (stroke->count == GRAFFITI_STROKE_POINT_MAX) {
        size_t kept = 0U;
        for (size_t i = 0; i < stroke->count; i += 2U) {
            stroke->points[kept++] = stroke->points[i];
        }
        stroke->count = (uint16_t)kept;
    }
    stroke->points[stroke->count++] = (solar_os_unistroke_point_t) {
        .x = (float)x,
        .y = (float)y,
    };
}

static uint16_t graffiti_job_display_width(
    const solar_os_input_pointer_event_t *event)
{
    const char *target_name = event->target[0] != '\0' ?
        event->target : SOLAR_OS_DISPLAY_PRIMARY_TARGET;
    solar_os_display_target_t target;
    if (!solar_os_display_find_target(target_name, &target)) {
        return 0U;
    }
    solar_os_terminal_profile_t profile;
    if (solar_os_display_get_terminal_profile(target_name, &profile) == ESP_OK &&
        (profile.orientation_degrees == 90U ||
         profile.orientation_degrees == 270U)) {
        return target.height;
    }
    return target.width;
}

static bool graffiti_job_filter(const solar_os_input_pointer_event_t *event,
                                void *context)
{
    graffiti_job_state_t *state = context;
    if (state == NULL || !state->running ||
        event->mode != SOLAR_OS_INPUT_POINTER_ABSOLUTE) {
        return false;
    }
    if (event->action == SOLAR_OS_INPUT_POINTER_PRESS) {
        state->tracking = false;
        state->display_width = graffiti_job_display_width(event);
        if (state->display_width > 0U) {
            state->tracking = true;
            state->pointer_id = event->pointer_id;
            state->active.count = 0U;
            state->active.zone = solar_os_graffiti_zone_for_start(
                event->x, state->display_width);
            graffiti_job_add_point(&state->active, event->x, event->y);
        } else {
            state->dropped++;
        }
        return false;
    }
    if (!state->tracking || event->pointer_id != state->pointer_id) {
        return false;
    }
    graffiti_job_add_point(&state->active, event->x, event->y);
    if (event->action == SOLAR_OS_INPUT_POINTER_RELEASE) {
        state->tracking = false;
        if (state->active.count < 2U ||
            xQueueSend(state->queue, &state->active, 0) != pdTRUE) {
            state->dropped++;
        }
    }
    return false;
}

static void graffiti_job_worker(void *argument)
{
    graffiti_job_state_t *state = argument;
    while (state->running || uxQueueMessagesWaiting(state->queue) > 0U) {
        graffiti_stroke_t stroke;
        if (xQueueReceive(state->queue, &stroke, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }
        solar_os_graffiti_result_t result;
        if (!solar_os_graffiti_recognize(state->recognizer, stroke.zone,
                                         stroke.points, stroke.count, &result) ||
            !result.matched) {
            state->rejected++;
            continue;
        }
        char character = '\0';
        if (solar_os_graffiti_apply(&state->case_state, &result, &character)) {
            if (solar_os_input_write_char(state->keyboard_source, character) != ESP_OK) {
                state->dropped++;
                continue;
            }
        }
        state->recognized++;
    }
    state->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static bool graffiti_job_has_absolute_touch(void)
{
    for (size_t i = 0; i < solar_os_input_source_count(); i++) {
        solar_os_input_source_info_t source;
        if (solar_os_input_source_get(i, &source) && source.ready &&
            source.source_class == SOLAR_OS_INPUT_SOURCE_TOUCH &&
            (source.capabilities & SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE) != 0U) {
            return true;
        }
    }
    return false;
}

static void graffiti_job_release_resources(void)
{
    if (graffiti_job.keyboard_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_release_all(graffiti_job.keyboard_source);
        solar_os_input_source_close(graffiti_job.keyboard_source);
        graffiti_job.keyboard_source = SOLAR_OS_INPUT_SOURCE_INVALID;
    }
    solar_os_queue_delete(graffiti_job.queue);
    graffiti_job.queue = NULL;
    solar_os_memory_free(graffiti_job.recognizer);
    graffiti_job.recognizer = NULL;
}

static esp_err_t graffiti_job_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!graffiti_job_has_absolute_touch()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    memset(&graffiti_job, 0, sizeof(graffiti_job));
    graffiti_job.recognizer = solar_os_memory_calloc(
        1U, solar_os_graffiti_context_size(),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "graffiti-templates");
    if (graffiti_job.recognizer == NULL ||
        !solar_os_graffiti_init(graffiti_job.recognizer,
                                solar_os_graffiti_context_size())) {
        graffiti_job_release_resources();
        return ESP_ERR_NO_MEM;
    }
    graffiti_job.queue = solar_os_queue_create(GRAFFITI_QUEUE_DEPTH,
                                                sizeof(graffiti_stroke_t));
    if (graffiti_job.queue == NULL) {
        graffiti_job_release_resources();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = solar_os_input_keyboard_source_open(
        "graffiti", true, &graffiti_job.keyboard_source);
    if (err != ESP_OK) {
        graffiti_job_release_resources();
        return err;
    }
    graffiti_job.running = true;
    err = solar_os_input_pointer_filter_register(graffiti_job_filter,
                                                  &graffiti_job);
    if (err != ESP_OK) {
        graffiti_job.running = false;
        graffiti_job_release_resources();
        return err;
    }
    if (solar_os_task_create_pinned_internal(
            graffiti_job_worker, "graffiti_worker", GRAFFITI_WORKER_STACK,
            &graffiti_job, GRAFFITI_WORKER_PRIORITY,
            &graffiti_job.worker_task, tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        solar_os_input_pointer_filter_unregister(graffiti_job_filter,
                                                  &graffiti_job);
        graffiti_job.running = false;
        graffiti_job_release_resources();
        return ESP_ERR_NO_MEM;
    }
    (void)solar_os_jobs_note_resource("graffiti",
                                      SOLAR_OS_JOB_RESOURCE_CUSTOM,
                                      "pointer-observer", "absolute touch");
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io != NULL) {
        solar_os_shell_io_printf(
            io, "graffiti started: letters left 2/3, numbers right 1/3\n");
    }
    return ESP_OK;
}

static void graffiti_job_stop(solar_os_context_t *ctx)
{
    if (!graffiti_job.running && graffiti_job.worker_task == NULL) {
        return;
    }
    solar_os_input_pointer_filter_unregister(graffiti_job_filter, &graffiti_job);
    graffiti_job.running = false;
    if (!solar_os_task_wait_done(graffiti_job.worker_task,
                                 &graffiti_job.worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return;
    }
    graffiti_job.worker_task = NULL;
    graffiti_job.worker_done = false;
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io != NULL) {
        solar_os_shell_io_printf(io,
                                 "graffiti stopped: %lu recognized, %lu rejected, %lu dropped\n",
                                 (unsigned long)graffiti_job.recognized,
                                 (unsigned long)graffiti_job.rejected,
                                 (unsigned long)graffiti_job.dropped);
    }
    graffiti_job_release_resources();
}

static void graffiti_job_detail(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
    solar_os_shell_io_printf(io,
                             "  strokes: recognized=%lu rejected=%lu dropped=%lu\n",
                             (unsigned long)graffiti_job.recognized,
                             (unsigned long)graffiti_job.rejected,
                             (unsigned long)graffiti_job.dropped);
    solar_os_shell_io_printf(io, "  case: %s\n",
                             graffiti_job.case_state.caps_lock ? "caps-lock" :
                             graffiti_job.case_state.shift_pending ? "shift" : "lower");
}

const solar_os_job_t solar_os_graffiti_job = {
    .name = "graffiti",
    .summary = "Palm Graffiti full-screen touch keyboard",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = graffiti_job_start,
    .stop = graffiti_job_stop,
    .worker_stack_bytes = GRAFFITI_WORKER_STACK,
    .detail = graffiti_job_detail,
};
