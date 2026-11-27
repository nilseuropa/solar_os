#include "solar_os_display_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"

#define DISPLAY_LAYOUT_STACK 3072U
#define DISPLAY_LAYOUT_PRIORITY (tskIDLE_PRIORITY + 1U)
#define DISPLAY_LAYOUT_COALESCE_MS 20U
#define DISPLAY_LAYOUT_STOP_WAIT_MS 15000U
#define DISPLAY_LAYOUT_OWNER_PREFIX "display-layout:"

typedef struct solar_os_display_layout_runtime solar_os_display_layout_runtime_t;

typedef struct {
    solar_os_display_layout_runtime_t *layout;
    u8g2_t u8g2;
    u8x8_display_info_t display_info;
    uint8_t *buffer;
    size_t buffer_size;
    solar_os_display_layout_rect_t region;
    char name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    bool registered;
} display_layout_logical_t;

typedef struct {
    char name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    solar_os_display_layout_rect_t region;
    bool claimed;
} display_layout_backing_t;

typedef struct {
    display_layout_backing_t backing[SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX];
    display_layout_logical_t logical[SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX];
} display_layout_members_t;

struct solar_os_display_layout_runtime {
    bool active;
    bool reserved;
    bool destroying;
    solar_os_display_layout_kind_t kind;
    solar_os_display_layout_axis_t axis;
    char name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
    uint16_t width;
    uint16_t height;
    size_t canvas_size;
    uint8_t *queued;
    uint8_t *presenting;
    display_layout_members_t *members;
    size_t backing_count;
    size_t logical_count;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    SemaphoreHandle_t requested;
    StaticSemaphore_t requested_storage;
    TaskHandle_t task;
    volatile bool task_done;
    bool stop_requested;
    bool queued_ready;
};

static const char *TAG = "display_layout";
static solar_os_display_layout_runtime_t layouts[SOLAR_OS_DISPLAY_LAYOUT_MAX];
static portMUX_TYPE layouts_lock = portMUX_INITIALIZER_UNLOCKED;

static bool layout_name_valid(const char *name)
{
    return name != NULL && name[0] != '\0' &&
        strnlen(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) <
            SOLAR_OS_DISPLAY_TARGET_NAME_MAX;
}

static bool layout_axis_valid(solar_os_display_layout_axis_t axis)
{
    return axis == SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL ||
        axis == SOLAR_OS_DISPLAY_LAYOUT_VERTICAL;
}

const char *solar_os_display_layout_axis_name(
    solar_os_display_layout_axis_t axis)
{
    return axis == SOLAR_OS_DISPLAY_LAYOUT_VERTICAL ? "vertical" :
        "horizontal";
}

static esp_err_t layout_reserve_create(const char *name, int *slot)
{
    if (slot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *slot = -1;
    portENTER_CRITICAL(&layouts_lock);
    for (size_t i = 0U; i < SOLAR_OS_DISPLAY_LAYOUT_MAX; i++) {
        if ((layouts[i].active || layouts[i].reserved) &&
            strcmp(layouts[i].name, name) == 0) {
            portEXIT_CRITICAL(&layouts_lock);
            return ESP_ERR_INVALID_STATE;
        }
    }
    for (size_t i = 0U; i < SOLAR_OS_DISPLAY_LAYOUT_MAX; i++) {
        if (!layouts[i].active && !layouts[i].reserved &&
            layouts[i].task == NULL &&
            layouts[i].queued == NULL && layouts[i].presenting == NULL &&
            layouts[i].members == NULL) {
            layouts[i].reserved = true;
            strlcpy(layouts[i].name, name, sizeof(layouts[i].name));
            *slot = (int)i;
            portEXIT_CRITICAL(&layouts_lock);
            /* Reserve the internal control slot first; allocate outside the
             * spinlock. Synchronization provider storage never moves. */
            layouts[i].members = solar_os_memory_calloc(
                1U, sizeof(*layouts[i].members),
                SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "display.layout.members");
            if (layouts[i].members == NULL) {
                portENTER_CRITICAL(&layouts_lock);
                layouts[i].reserved = false;
                layouts[i].name[0] = '\0';
                portEXIT_CRITICAL(&layouts_lock);
                *slot = -1;
                return ESP_ERR_NO_MEM;
            }
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&layouts_lock);
    return ESP_ERR_NO_MEM;
}

static int layout_reserve_destroy(solar_os_display_layout_kind_t kind,
                                  const char *name)
{
    portENTER_CRITICAL(&layouts_lock);
    for (size_t i = 0U; i < SOLAR_OS_DISPLAY_LAYOUT_MAX; i++) {
        if (layouts[i].active && !layouts[i].destroying &&
            layouts[i].kind == kind && strcmp(layouts[i].name, name) == 0) {
            layouts[i].destroying = true;
            portEXIT_CRITICAL(&layouts_lock);
            return (int)i;
        }
    }
    portEXIT_CRITICAL(&layouts_lock);
    return -1;
}

static void layout_activate(solar_os_display_layout_runtime_t *layout)
{
    portENTER_CRITICAL(&layouts_lock);
    layout->active = true;
    layout->reserved = false;
    portEXIT_CRITICAL(&layouts_lock);
}

static int compare_names(const void *left, const void *right)
{
    const char *const *left_name = left;
    const char *const *right_name = right;
    return strcmp(*left_name, *right_name);
}

static bool layout_submit(display_layout_logical_t *logical)
{
    solar_os_display_layout_runtime_t *layout = logical->layout;
    if (layout == NULL || layout->mutex == NULL || layout->requested == NULL ||
        xSemaphoreTake(layout->mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool accepted = !layout->stop_requested &&
        solar_os_display_layout_mono_blit(
            layout->queued, layout->width, layout->height,
            logical->region.x, logical->region.y, logical->buffer,
            logical->region.width, logical->region.height);
    if (accepted) {
        layout->queued_ready = true;
    }
    xSemaphoreGive(layout->mutex);
    if (accepted) {
        (void)xSemaphoreGive(layout->requested);
    }
    return accepted;
}

static uint8_t layout_display_cb(u8x8_t *u8x8,
                                 uint8_t msg,
                                 uint8_t arg_int,
                                 void *arg_ptr)
{
    (void)arg_int;
    (void)arg_ptr;
    display_layout_logical_t *logical =
        (display_layout_logical_t *)((uint8_t *)u8x8 -
            offsetof(display_layout_logical_t, u8g2));
    if (msg == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &logical->display_info);
        return 1U;
    }
    return msg != U8X8_MSG_DISPLAY_REFRESH || layout_submit(logical);
}

static esp_err_t layout_present_frame(
    void *context, const solar_os_display_raster_t *frame)
{
    display_layout_logical_t *logical = context;
    if (logical == NULL || frame == NULL || frame->data == NULL ||
        frame->format != SOLAR_OS_DISPLAY_FORMAT_MONO1 ||
        frame->width == 0U || frame->height == 0U ||
        frame->source_width != frame->width ||
        frame->source_height != frame->height ||
        frame->source_stride < ((size_t)frame->width + 7U) / 8U ||
        frame->height > SIZE_MAX / frame->source_stride ||
        frame->data_size < (size_t)frame->height * frame->source_stride ||
        (uint32_t)frame->x + frame->width > u8g2_GetDisplayWidth(&logical->u8g2) ||
        (uint32_t)frame->y + frame->height > u8g2_GetDisplayHeight(&logical->u8g2) ||
        (frame->clear_background && frame->background_index > 1U)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The display service scales MONO1 rasters before calling us. Native
     * layout buffers use one for white, like the logical text canvas. */
    const uint8_t inversion = frame->palette_inverted ? 0U : 0xffU;
    if (frame->clear_background) {
        memset(logical->buffer,
            (frame->background_index != 0U) != frame->palette_inverted ? 0x00 : 0xff,
            logical->buffer_size);
    }
    if (logical->u8g2.cb == U8G2_R0) {
        const size_t stride = ((size_t)logical->region.width + 7U) & ~(size_t)7U;
        for (unsigned y = 0U; y < frame->height;) {
            const unsigned dy = frame->y + y, shift = dy & 7U;
            const unsigned count = frame->height - y < 8U - shift ?
                frame->height - y : 8U - shift;
            const uint8_t mask = (uint8_t)(((1U << count) - 1U) << shift);
            uint8_t *out = logical->buffer + (dy / 8U) * stride + frame->x;
            const uint8_t *rows[8];
            for (unsigned row = 0U; row < count; row++)
                rows[row] = frame->data + (size_t)(y + row) * frame->source_stride;
            for (unsigned x = 0U; x < frame->width; x++) {
                unsigned bits = 0U;
                for (unsigned row = 0U; row < count; row++)
                    bits |= ((rows[row][x / 8U] >> (x & 7U)) & 1U) << row;
                out[x] = (out[x] & (uint8_t)~mask) |
                    (((bits ^ inversion) << shift) & mask);
            }
            y += count;
        }
    } else {
        /* u8g2 maps oriented pixels back into the native layout buffer. */
        for (unsigned y = 0U; y < frame->height; y++) {
            const uint8_t *row = frame->data + (size_t)y * frame->source_stride;
            for (unsigned x = 0U; x < frame->width; x++) {
                u8g2_SetDrawColor(&logical->u8g2,
                    ((row[x / 8U] >> (x & 7U)) ^ inversion) & 1U);
                u8g2_DrawPixel(&logical->u8g2, frame->x + x, frame->y + y);
            }
        }
        u8g2_SetDrawColor(&logical->u8g2, 1U);
    }
    return layout_submit(logical) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t layout_present_backing(
    solar_os_display_layout_runtime_t *layout,
    const display_layout_backing_t *backing)
{
    solar_os_display_target_t target;
    if (!solar_os_display_find_target(backing->name, &target)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!target.ready || target.u8g2 == NULL ||
        target.width != backing->region.width ||
        target.height != backing->region.height) {
        return ESP_ERR_INVALID_STATE;
    }

    u8g2_t *u8g2 = target.u8g2;
    /* The normal unrotated full buffer already uses the layout's packed
     * format. Avoid redrawing every set pixel through u8g2 on each update.
     * Rotated and paged canvases retain the coordinate-aware drawing path. */
    if (u8g2->cb == U8G2_R0 &&
        u8g2->pixel_buf_width == ((target.width + 7U) & ~7U) &&
        (size_t)u8g2_GetBufferTileHeight(u8g2) * 8U >= target.height &&
        solar_os_display_layout_mono_extract(
            u8g2_GetBufferPtr(u8g2), target.width, target.height,
            layout->presenting, layout->width, layout->height,
            backing->region.x, backing->region.y)) {
        u8g2_SetDrawColor(u8g2, 1U);
        solar_os_display_present(u8g2, SOLAR_OS_DISPLAY_PRESENT_GRAPHICS);
        return ESP_OK;
    }
    u8g2_SetDrawColor(u8g2, 0U);
    u8g2_DrawBox(u8g2, 0U, 0U, target.width, target.height);
    u8g2_SetDrawColor(u8g2, 1U);
    for (uint16_t y = 0U; y < target.height; y++) {
        for (uint16_t x = 0U; x < target.width; x++) {
            if (solar_os_display_layout_mono_get(
                    layout->presenting, layout->width, layout->height,
                    (uint16_t)(backing->region.x + x),
                    (uint16_t)(backing->region.y + y))) {
                u8g2_DrawPixel(u8g2, x, y);
            }
        }
    }
    solar_os_display_present(u8g2, SOLAR_OS_DISPLAY_PRESENT_GRAPHICS);
    return ESP_OK;
}

static void layout_worker(void *arg)
{
    solar_os_display_layout_runtime_t *layout = arg;
    while (true) {
        (void)xSemaphoreTake(layout->requested, portMAX_DELAY);
        if (xSemaphoreTake(layout->mutex, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const bool stop = layout->stop_requested;
        xSemaphoreGive(layout->mutex);
        if (stop) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(DISPLAY_LAYOUT_COALESCE_MS));
        while (xSemaphoreTake(layout->requested, 0U) == pdTRUE) {
        }
        if (xSemaphoreTake(layout->mutex, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (layout->stop_requested) {
            xSemaphoreGive(layout->mutex);
            break;
        }
        if (!layout->queued_ready) {
            xSemaphoreGive(layout->mutex);
            continue;
        }
        uint8_t *swap = layout->presenting;
        layout->presenting = layout->queued;
        layout->queued = swap;
        memcpy(layout->queued, layout->presenting, layout->canvas_size);
        layout->queued_ready = false;
        xSemaphoreGive(layout->mutex);

        for (size_t i = 0U; i < layout->backing_count; i++) {
            const esp_err_t err = layout_present_backing(layout,
                                                         &layout->members->backing[i]);
            if (err != ESP_OK) {
                SOLAR_OS_LOGW(TAG, "present %s failed: %s",
                              layout->members->backing[i].name, esp_err_to_name(err));
            }
        }
    }
    layout->task_done = true;
    solar_os_task_delete_internal(NULL);
}

static esp_err_t layout_start_worker(solar_os_display_layout_runtime_t *layout)
{
    layout->task_done = false;
    layout->stop_requested = false;
    const BaseType_t created = solar_os_task_create_pinned_internal(
        layout_worker, "display_layout", DISPLAY_LAYOUT_STACK, layout,
        DISPLAY_LAYOUT_PRIORITY, &layout->task, tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_BACKGROUND);
    if (created != pdPASS) {
        layout->task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t layout_stop_worker(solar_os_display_layout_runtime_t *layout)
{
    if (layout->task == NULL) {
        return ESP_OK;
    }
    if (xSemaphoreTake(layout->mutex, portMAX_DELAY) == pdTRUE) {
        layout->stop_requested = true;
        xSemaphoreGive(layout->mutex);
    }
    (void)xSemaphoreGive(layout->requested);
    if (!solar_os_task_wait_done(layout->task, &layout->task_done,
                                 DISPLAY_LAYOUT_STOP_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    layout->task = NULL;
    return ESP_OK;
}

static void layout_release_backing(solar_os_display_layout_runtime_t *layout)
{
    for (size_t i = 0U; i < layout->backing_count; i++) {
        if (layout->members->backing[i].claimed) {
            (void)solar_os_display_release(layout->members->backing[i].name,
                                           layout->owner);
            layout->members->backing[i].claimed = false;
        }
    }
}

static void layout_free(solar_os_display_layout_runtime_t *layout)
{
    /* Stop new snapshots before releasing member metadata. The reserved
     * slot cannot be reused until teardown has completed. */
    portENTER_CRITICAL(&layouts_lock);
    layout->active = false;
    layout->reserved = true;
    portEXIT_CRITICAL(&layouts_lock);
    for (size_t i = 0U; i < layout->logical_count; i++) {
        solar_os_memory_free(layout->members->logical[i].buffer);
        layout->members->logical[i].buffer = NULL;
    }
    solar_os_memory_free(layout->queued);
    solar_os_memory_free(layout->presenting);
    layout->queued = NULL;
    layout->presenting = NULL;
    if (layout->requested != NULL) {
        vSemaphoreDelete(layout->requested);
    }
    if (layout->mutex != NULL) {
        vSemaphoreDelete(layout->mutex);
    }
    solar_os_memory_free(layout->members);
    portENTER_CRITICAL(&layouts_lock);
    memset(layout, 0, sizeof(*layout));
    portEXIT_CRITICAL(&layouts_lock);
}

static esp_err_t layout_rollback(solar_os_display_layout_runtime_t *layout)
{
    const char *registered[SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX];
    size_t registered_count = 0U;
    for (size_t i = 0U; i < layout->logical_count; i++) {
        if (layout->members->logical[i].registered) {
            registered[registered_count++] = layout->members->logical[i].name;
        }
    }
    if (registered_count > 0U) {
        const esp_err_t err = solar_os_display_unregister_targets(
            registered, registered_count);
        if (err != ESP_OK) {
            /* Publish the retained slot so unjoin/unsplit can retry cleanup. */
            layout_activate(layout);
            return err;
        }
        for (size_t i = 0U; i < layout->logical_count; i++) {
            layout->members->logical[i].registered = false;
        }
    }
    const esp_err_t stop_err = layout_stop_worker(layout);
    if (stop_err != ESP_OK) {
        layout_activate(layout);
        return stop_err;
    }
    layout_release_backing(layout);
    layout_free(layout);
    return ESP_OK;
}

static esp_err_t layout_prepare_logical(
    solar_os_display_layout_runtime_t *layout,
    size_t index,
    const char *name,
    solar_os_display_layout_rect_t region,
    const char *role)
{
    if (!layout_name_valid(name) || region.width == 0U || region.height == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t tile_width = (uint16_t)((region.width + 7U) / 8U);
    const uint16_t tile_height = (uint16_t)((region.height + 7U) / 8U);
    if (tile_width > UINT8_MAX || tile_height > UINT8_MAX) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    display_layout_logical_t *logical = &layout->members->logical[index];
    logical->layout = layout;
    logical->region = region;
    strlcpy(logical->name, name, sizeof(logical->name));
    logical->buffer_size = solar_os_display_layout_mono_size(region.width,
                                                              region.height);
    logical->buffer = solar_os_memory_calloc(
        1U, logical->buffer_size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "display.layout.fb");
    if (logical->buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    logical->display_info = (u8x8_display_info_t){
        .chip_enable_level = 0U,
        .chip_disable_level = 1U,
        .sck_clock_hz = 4000000UL,
        .i2c_bus_clock_100kHz = 4U,
        .tile_width = (uint8_t)tile_width,
        .tile_height = (uint8_t)tile_height,
        .pixel_width = region.width,
        .pixel_height = region.height,
    };
    u8g2_SetupDisplay(&logical->u8g2, layout_display_cb, u8x8_cad_empty,
                      u8x8_dummy_cb, u8x8_dummy_cb);
    u8g2_SetupBuffer(&logical->u8g2, logical->buffer, (uint8_t)tile_height,
                     u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    u8g2_InitDisplay(&logical->u8g2);
    u8g2_ClearBuffer(&logical->u8g2);

    solar_os_display_target_t target = {0};
    strlcpy(target.name, name, sizeof(target.name));
    strlcpy(target.source, "layout", sizeof(target.source));
    strlcpy(target.driver, "compositor", sizeof(target.driver));
    strlcpy(target.role, role, sizeof(target.role));
    target.width = region.width;
    target.height = region.height;
    target.ready = true;
    target.black_is_one = false;
    target.u8g2 = &logical->u8g2;
    target.frame_formats = SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT;
    target.preferred_stream_fps = 25U;
    target.frame_context = logical;
    target.present_frame = layout_present_frame;
    const esp_err_t err = solar_os_display_register_target(&target);
    if (err == ESP_OK) {
        logical->registered = true;
    }
    return err;
}

static esp_err_t layout_claim_backing(
    solar_os_display_layout_runtime_t *layout,
    char *busy_owner,
    size_t busy_owner_len)
{
    const char *ordered[SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX];
    for (size_t i = 0U; i < layout->backing_count; i++) {
        ordered[i] = layout->members->backing[i].name;
    }
    qsort(ordered, layout->backing_count, sizeof(ordered[0]), compare_names);
    for (size_t ordered_index = 0U; ordered_index < layout->backing_count;
         ordered_index++) {
        const esp_err_t err = solar_os_display_claim(
            ordered[ordered_index], layout->owner, busy_owner, busy_owner_len);
        if (err != ESP_OK) {
            return err;
        }
        for (size_t i = 0U; i < layout->backing_count; i++) {
            if (strcmp(layout->members->backing[i].name, ordered[ordered_index]) == 0) {
                layout->members->backing[i].claimed = true;
                break;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t layout_initialize(solar_os_display_layout_runtime_t *layout)
{
    layout->canvas_size = solar_os_display_layout_mono_size(layout->width,
                                                            layout->height);
    if (layout->canvas_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    layout->queued = solar_os_memory_calloc(
        1U, layout->canvas_size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "display.layout.queue");
    layout->presenting = solar_os_memory_calloc(
        1U, layout->canvas_size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "display.layout.show");
    if (layout->queued == NULL || layout->presenting == NULL) {
        return ESP_ERR_NO_MEM;
    }
    layout->mutex = xSemaphoreCreateMutexStatic(&layout->mutex_storage);
    layout->requested = xSemaphoreCreateBinaryStatic(&layout->requested_storage);
    return layout->mutex != NULL && layout->requested != NULL ?
        ESP_OK : ESP_ERR_NO_MEM;
}

static bool layout_backing_valid(const char *name,
                                 solar_os_display_target_t *target)
{
    return layout_name_valid(name) &&
        solar_os_display_find_target(name, target) && target->ready &&
        target->u8g2 != NULL && strcmp(target->source, "layout") != 0;
}

static esp_err_t layout_join(
    const char *name,
    solar_os_display_layout_axis_t axis,
    size_t target_count,
    const char *const *targets,
    char *busy_owner,
    size_t busy_owner_len)
{
    if (!layout_name_valid(name) || !layout_axis_valid(axis) ||
        targets == NULL || target_count < 2U ||
        target_count > SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX ||
        solar_os_display_find_target(name, &(solar_os_display_target_t){0})) {
        return ESP_ERR_INVALID_ARG;
    }
    int slot = -1;
    const esp_err_t reserve_err = layout_reserve_create(name, &slot);
    if (reserve_err != ESP_OK) {
        return reserve_err;
    }
    solar_os_display_layout_runtime_t *layout = &layouts[slot];
    layout->kind = SOLAR_OS_DISPLAY_LAYOUT_JOIN;
    layout->axis = axis;
    layout->backing_count = target_count;
    layout->logical_count = 1U;
    snprintf(layout->owner, sizeof(layout->owner), "%s%s",
             DISPLAY_LAYOUT_OWNER_PREFIX, name);

    uint16_t widths[SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX];
    uint16_t heights[SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX];
    solar_os_display_layout_rect_t regions[SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX];
    for (size_t i = 0U; i < target_count; i++) {
        solar_os_display_target_t target;
        if (!layout_backing_valid(targets[i], &target)) {
            layout_free(layout);
            return ESP_ERR_NOT_FOUND;
        }
        for (size_t previous = 0U; previous < i; previous++) {
            if (strcmp(targets[previous], targets[i]) == 0) {
                layout_free(layout);
                return ESP_ERR_INVALID_ARG;
            }
        }
        strlcpy(layout->members->backing[i].name, targets[i],
                sizeof(layout->members->backing[i].name));
        widths[i] = target.width;
        heights[i] = target.height;
    }
    if (!solar_os_display_layout_join_geometry(
            axis, widths, heights, target_count, &layout->width,
            &layout->height, regions)) {
        layout_free(layout);
        return ESP_ERR_NOT_SUPPORTED;
    }
    for (size_t i = 0U; i < target_count; i++) {
        layout->members->backing[i].region = regions[i];
    }

    esp_err_t err = layout_initialize(layout);
    if (err == ESP_OK) {
        err = layout_claim_backing(layout, busy_owner, busy_owner_len);
    }
    if (err == ESP_OK) {
        err = layout_start_worker(layout);
    }
    if (err == ESP_OK) {
        err = layout_prepare_logical(
            layout, 0U, name,
            (solar_os_display_layout_rect_t){0U, 0U, layout->width,
                                             layout->height},
            "joined");
    }
    if (err != ESP_OK) {
        const esp_err_t cleanup_err = layout_rollback(layout);
        return cleanup_err != ESP_OK ? cleanup_err : err;
    }
    layout_activate(layout);
    return ESP_OK;
}

static esp_err_t layout_split(
    const char *target_name,
    solar_os_display_layout_axis_t axis,
    const char *first,
    const char *second,
    char *busy_owner,
    size_t busy_owner_len)
{
    if (!layout_name_valid(target_name) || !layout_name_valid(first) ||
        !layout_name_valid(second) || !layout_axis_valid(axis) ||
        strcmp(first, second) == 0 || strcmp(first, target_name) == 0 ||
        strcmp(second, target_name) == 0 ||
        solar_os_display_find_target(first, &(solar_os_display_target_t){0}) ||
        solar_os_display_find_target(second, &(solar_os_display_target_t){0})) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_display_target_t target;
    if (!layout_backing_valid(target_name, &target)) {
        return ESP_ERR_NOT_FOUND;
    }
    int slot = -1;
    const esp_err_t reserve_err = layout_reserve_create(target_name, &slot);
    if (reserve_err != ESP_OK) {
        return reserve_err;
    }
    solar_os_display_layout_runtime_t *layout = &layouts[slot];
    layout->kind = SOLAR_OS_DISPLAY_LAYOUT_SPLIT;
    layout->axis = axis;
    layout->width = target.width;
    layout->height = target.height;
    layout->backing_count = 1U;
    layout->logical_count = 2U;
    snprintf(layout->owner, sizeof(layout->owner), "%s%s",
             DISPLAY_LAYOUT_OWNER_PREFIX, target_name);
    strlcpy(layout->members->backing[0].name, target_name,
            sizeof(layout->members->backing[0].name));
    layout->members->backing[0].region = (solar_os_display_layout_rect_t){
        0U, 0U, target.width, target.height};

    solar_os_display_layout_rect_t regions[2];
    if (!solar_os_display_layout_split_geometry(axis, target.width,
                                                target.height, regions)) {
        layout_free(layout);
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = layout_initialize(layout);
    if (err == ESP_OK) {
        err = layout_claim_backing(layout, busy_owner, busy_owner_len);
    }
    if (err == ESP_OK) {
        err = layout_start_worker(layout);
    }
    if (err == ESP_OK) {
        err = layout_prepare_logical(layout, 0U, first, regions[0], "viewport");
    }
    if (err == ESP_OK) {
        err = layout_prepare_logical(layout, 1U, second, regions[1], "viewport");
    }
    if (err != ESP_OK) {
        const esp_err_t cleanup_err = layout_rollback(layout);
        return cleanup_err != ESP_OK ? cleanup_err : err;
    }
    layout_activate(layout);
    return ESP_OK;
}

static esp_err_t layout_destroy(solar_os_display_layout_runtime_t *layout)
{
    const char *names[SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX];
    size_t name_count = 0U;
    for (size_t i = 0U; i < layout->logical_count; i++) {
        if (layout->members->logical[i].registered) {
            names[name_count++] = layout->members->logical[i].name;
        }
    }
    if (name_count > 0U) {
        const esp_err_t unregister_err = solar_os_display_unregister_targets(
            names, name_count);
        if (unregister_err != ESP_OK) {
            return unregister_err;
        }
        for (size_t i = 0U; i < layout->logical_count; i++) {
            layout->members->logical[i].registered = false;
        }
    }
    const esp_err_t stop_err = layout_stop_worker(layout);
    if (stop_err != ESP_OK) {
        return stop_err;
    }
    layout_release_backing(layout);
    layout_free(layout);
    return ESP_OK;
}

esp_err_t solar_os_display_layout_create(const solar_os_display_layout_request_v1_t *request,
                                         char *busy_owner, size_t capacity)
{
    if (busy_owner && capacity) busy_owner[0] = '\0';
    if (!request || !request->members) return ESP_ERR_INVALID_ARG;
    switch (request->kind) {
    case SOLAR_OS_DISPLAY_LAYOUT_JOIN:
        return layout_join(request->name, request->axis, request->member_count, request->members,
                           busy_owner, capacity);
    case SOLAR_OS_DISPLAY_LAYOUT_SPLIT:
        if (request->member_count != 2) return ESP_ERR_INVALID_ARG;
        return layout_split(request->name, request->axis, request->members[0], request->members[1],
                            busy_owner, capacity);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static bool layout_kind_valid(solar_os_display_layout_kind_t kind)
{
    return kind == SOLAR_OS_DISPLAY_LAYOUT_JOIN || kind == SOLAR_OS_DISPLAY_LAYOUT_SPLIT;
}

esp_err_t solar_os_display_layout_remove(solar_os_display_layout_kind_t kind, const char *name)
{
    if (!layout_kind_valid(kind) || !layout_name_valid(name)) return ESP_ERR_INVALID_ARG;
    const int slot = layout_reserve_destroy(kind, name);
    if (slot < 0) return ESP_ERR_NOT_FOUND;
    const esp_err_t error = layout_destroy(&layouts[slot]);
    if (error != ESP_OK) {
        portENTER_CRITICAL(&layouts_lock);
        layouts[slot].destroying = false;
        portEXIT_CRITICAL(&layouts_lock);
    }
    return error;
}

size_t solar_os_display_layout_count(void)
{
    size_t count = 0U;
    portENTER_CRITICAL(&layouts_lock);
    for (size_t i = 0U; i < SOLAR_OS_DISPLAY_LAYOUT_MAX; i++) {
        if (layouts[i].active) {
            count++;
        }
    }
    portEXIT_CRITICAL(&layouts_lock);
    return count;
}

static void layout_copy_info(const solar_os_display_layout_runtime_t *layout,
                             solar_os_display_layout_info_t *info)
{
    info->kind = layout->kind;
    info->axis = layout->axis;
    strlcpy(info->name, layout->name, sizeof(info->name));
    info->width = layout->width;
    info->height = layout->height;
    info->backing_count = layout->backing_count;
    info->logical_count = layout->logical_count;
    for (size_t i = 0; i < layout->backing_count; i++)
        strlcpy(info->backing[i], layout->members->backing[i].name, sizeof(info->backing[i]));
    for (size_t i = 0; i < layout->logical_count; i++)
        strlcpy(info->logical[i], layout->members->logical[i].name, sizeof(info->logical[i]));
}

bool solar_os_display_layout_get(size_t index, solar_os_display_layout_info_t *info)
{
    if (!info) return false;
    *info = (solar_os_display_layout_info_t){0};
    size_t current = 0;
    portENTER_CRITICAL(&layouts_lock);
    for (size_t i = 0; i < SOLAR_OS_DISPLAY_LAYOUT_MAX; i++) {
        const solar_os_display_layout_runtime_t *layout = &layouts[i];
        if (!layout->active || current++ != index) continue;
        layout_copy_info(layout, info);
        portEXIT_CRITICAL(&layouts_lock);
        return true;
    }
    portEXIT_CRITICAL(&layouts_lock);
    return false;
}

esp_err_t solar_os_display_layout_find(solar_os_display_layout_kind_t kind, const char *name,
                                       solar_os_display_layout_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    *info = (solar_os_display_layout_info_t){0};
    if (!layout_kind_valid(kind) || !layout_name_valid(name)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&layouts_lock);
    for (size_t i = 0; i < SOLAR_OS_DISPLAY_LAYOUT_MAX; i++) {
        const solar_os_display_layout_runtime_t *layout = &layouts[i];
        if (!layout->active || layout->kind != kind || strcmp(layout->name, name)) continue;
        layout_copy_info(layout, info);
        portEXIT_CRITICAL(&layouts_lock);
        return ESP_OK;
    }
    portEXIT_CRITICAL(&layouts_lock);
    return ESP_ERR_NOT_FOUND;
}
