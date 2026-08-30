#include "solar_os_frame_presenter.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_display.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"

#define FRAME_PRESENTER_STACK 3072U
#define FRAME_PRESENTER_PRIORITY (tskIDLE_PRIORITY + 1U)
#define FRAME_PRESENTER_HPM_HZ_TENTHS 255U

struct solar_os_frame_presenter {
    solar_os_frame_presenter_config_t config;
    solar_os_display_target_t target;
    char target_name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    uint8_t *presenting;
    uint8_t *queued;
    uint8_t *mono;
    size_t source_size;
    size_t mono_size;
    uint16_t output_x;
    uint16_t output_y;
    uint16_t output_width;
    uint16_t output_height;
    uint16_t output_stride;
    uint16_t present_fps;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    SemaphoreHandle_t requested;
    StaticSemaphore_t requested_storage;
    TaskHandle_t task;
    volatile bool task_done;
    bool queued_ready;
    bool stop_requested;
    bool mono_fallback;
    bool high_refresh_active;
    uint8_t mono_luma[256];
    solar_os_frame_presenter_stats_t stats;
};

static const char *TAG = "solar_os_frame_presenter";
static const uint8_t bayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

static size_t frame_minimum_stride(solar_os_display_format_t format,
                                   uint16_t width)
{
    if (format == SOLAR_OS_DISPLAY_FORMAT_MONO1) {
        return ((size_t)width + 7U) / 8U;
    }
    if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX2) {
        return ((size_t)width + 3U) / 4U;
    }
    if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX8) {
        return width;
    }
    return 0U;
}

static bool frame_palette_valid(
    const solar_os_frame_presenter_config_t *config)
{
    if (config->format == SOLAR_OS_DISPLAY_FORMAT_MONO1) {
        return true;
    }
    const size_t required = config->format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 ?
        4U : 256U;
    return config->palette_rgb565 != NULL && config->palette_size >= required;
}

static uint8_t frame_palette_index(const solar_os_frame_presenter_t *presenter,
                                   uint16_t x,
                                   uint16_t y)
{
    const uint8_t *row = presenter->presenting +
        (size_t)y * presenter->config.stride;
    if (presenter->config.format == SOLAR_OS_DISPLAY_FORMAT_INDEX2) {
        return (uint8_t)((row[x >> 2U] >> ((x & 3U) * 2U)) & 3U);
    }
    if (presenter->config.format == SOLAR_OS_DISPLAY_FORMAT_INDEX8) {
        return row[x];
    }
    return (uint8_t)((row[x >> 3U] >> (x & 7U)) & 1U);
}

static uint8_t rgb565_luma(uint16_t color)
{
    const uint32_t red = ((color >> 11U) & 0x1fU) * 255U / 31U;
    const uint32_t green = ((color >> 5U) & 0x3fU) * 255U / 63U;
    const uint32_t blue = (color & 0x1fU) * 255U / 31U;
    return (uint8_t)((red * 77U + green * 150U + blue * 29U) >> 8U);
}

static void frame_convert_to_mono(solar_os_frame_presenter_t *presenter)
{
    memset(presenter->mono, 0, presenter->mono_size);
    uint16_t source_y = 0U;
    uint32_t y_accumulator = 0U;
    for (uint16_t y = 0; y < presenter->output_height; y++) {
        uint16_t source_x = 0U;
        uint32_t x_accumulator = 0U;
        for (uint16_t x = 0; x < presenter->output_width; x++) {
            const uint8_t index =
                frame_palette_index(presenter, source_x, source_y);
            const uint16_t darkness =
                (uint16_t)(255U - presenter->mono_luma[index]);
            if ((uint16_t)bayer4[y & 3U][x & 3U] * 16U < darkness) {
                presenter->mono[(size_t)y * presenter->output_stride +
                                (x >> 3U)] |=
                    (uint8_t)(1U << (x & 7U));
            }
            x_accumulator += presenter->config.width;
            while (x_accumulator >= presenter->output_width) {
                x_accumulator -= presenter->output_width;
                source_x++;
            }
        }
        y_accumulator += presenter->config.height;
        while (y_accumulator >= presenter->output_height) {
            y_accumulator -= presenter->output_height;
            source_y++;
        }
    }
}

static void frame_prepare_mono_palette(solar_os_frame_presenter_t *presenter)
{
    if (!presenter->mono_fallback) {
        return;
    }
    const size_t palette_size =
        presenter->config.format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 ? 4U : 256U;
    for (size_t i = 0; i < palette_size; i++) {
        /* INDEX2 is an ordinal shade ramp. Keep its dither independent of the
         * color-display theme, then apply the terminal polarity below. */
        presenter->mono_luma[i] =
            presenter->config.format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 ?
                (uint8_t)(255U - i * 85U) :
                rgb565_luma(presenter->config.palette_rgb565[i]);
    }
}

static esp_err_t frame_present(solar_os_frame_presenter_t *presenter)
{
    solar_os_display_raster_t frame = {
        .data = presenter->presenting,
        .data_size = presenter->source_size,
        .palette_rgb565 = presenter->config.palette_rgb565,
        .palette_size = presenter->config.palette_size,
        .source_width = presenter->config.width,
        .source_height = presenter->config.height,
        .source_stride = presenter->config.stride,
        .x = presenter->output_x,
        .y = presenter->output_y,
        .width = presenter->output_width,
        .height = presenter->output_height,
        .format = presenter->config.format,
        .palette_inverted =
            solar_os_gfx_palette_inverted(presenter->config.gfx),
    };
    if (presenter->mono_fallback) {
        frame_convert_to_mono(presenter);
        frame.data = presenter->mono;
        frame.data_size = presenter->mono_size;
        frame.palette_rgb565 = NULL;
        frame.palette_size = 0U;
        frame.source_width = presenter->output_width;
        frame.source_height = presenter->output_height;
        frame.source_stride = presenter->output_stride;
        frame.format = SOLAR_OS_DISPLAY_FORMAT_MONO1;
        frame.palette_inverted =
            solar_os_gfx_palette_inverted(presenter->config.gfx);
    }
    return solar_os_gfx_present_frame(presenter->config.gfx, &frame);
}

static void frame_presenter_worker(void *arg)
{
    solar_os_frame_presenter_t *presenter = arg;
    const int64_t period_us = 1000000LL / presenter->present_fps;
    int64_t next_present_us = esp_timer_get_time();
    while (true) {
        (void)xSemaphoreTake(presenter->requested, portMAX_DELAY);
        if (xSemaphoreTake(presenter->mutex, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const bool stop = presenter->stop_requested;
        xSemaphoreGive(presenter->mutex);
        if (stop) {
            break;
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us < next_present_us) {
            const TickType_t wait_ticks = pdMS_TO_TICKS(
                (uint32_t)((next_present_us - now_us + 999LL) / 1000LL));
            if (wait_ticks > 0) {
                vTaskDelay(wait_ticks);
            }
        }

        if (xSemaphoreTake(presenter->mutex, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (presenter->stop_requested) {
            xSemaphoreGive(presenter->mutex);
            break;
        }
        if (!presenter->queued_ready) {
            xSemaphoreGive(presenter->mutex);
            continue;
        }
        uint8_t *swap = presenter->presenting;
        presenter->presenting = presenter->queued;
        presenter->queued = swap;
        presenter->queued_ready = false;
        xSemaphoreGive(presenter->mutex);

        const int64_t started_us = esp_timer_get_time();
        const esp_err_t err = frame_present(presenter);
        const uint64_t elapsed_us =
            (uint64_t)(esp_timer_get_time() - started_us);
        if (xSemaphoreTake(presenter->mutex, portMAX_DELAY) == pdTRUE) {
            presenter->stats.last_error = err;
            if (err == ESP_OK) {
                presenter->stats.present_us += elapsed_us;
                presenter->stats.presented_frames++;
            }
            xSemaphoreGive(presenter->mutex);
        }
        if (err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "present failed: %s", esp_err_to_name(err));
        }
        now_us = esp_timer_get_time();
        next_present_us += period_us;
        if (next_present_us < now_us) {
            next_present_us = now_us;
        }

        if (xSemaphoreTake(presenter->mutex, portMAX_DELAY) == pdTRUE) {
            const bool pending = presenter->queued_ready;
            xSemaphoreGive(presenter->mutex);
            if (pending) {
                (void)xSemaphoreGive(presenter->requested);
            }
        }
    }
    presenter->task_done = true;
    solar_os_task_delete_internal(NULL);
}

static void frame_choose_output(solar_os_frame_presenter_t *presenter)
{
    const uint16_t display_width = (uint16_t)solar_os_gfx_width(
        presenter->config.gfx);
    const uint16_t display_height = (uint16_t)solar_os_gfx_height(
        presenter->config.gfx);
    if (presenter->config.fit == SOLAR_OS_FRAME_FIT_HEIGHT) {
        uint32_t output_height = display_height;
        uint32_t output_width =
            ((uint32_t)presenter->config.width * output_height +
             presenter->config.height / 2U) / presenter->config.height;
        if (output_width > display_width) {
            output_width = display_width;
            output_height =
                ((uint32_t)presenter->config.height * output_width +
                 presenter->config.width / 2U) / presenter->config.width;
        }
        presenter->output_width = (uint16_t)output_width;
        presenter->output_height = (uint16_t)output_height;
        presenter->output_x =
            (uint16_t)((display_width - presenter->output_width) / 2U);
        presenter->output_y =
            (uint16_t)((display_height - presenter->output_height) / 2U);
        return;
    }
    uint16_t scale_x2 = (uint16_t)(
        (uint32_t)display_width * 2U / presenter->config.width);
    const uint16_t height_scale_x2 = (uint16_t)(
        (uint32_t)display_height * 2U / presenter->config.height);
    if (scale_x2 > height_scale_x2) scale_x2 = height_scale_x2;
    if (scale_x2 > 6U) scale_x2 = 6U;
    if (scale_x2 < 2U) scale_x2 = 2U;
    while (scale_x2 > 2U &&
           presenter->target.max_stream_pixels_per_second != 0U) {
        const uint32_t candidate_width =
            (uint32_t)presenter->config.width * scale_x2 / 2U;
        const uint32_t candidate_height =
            (uint32_t)presenter->config.height * scale_x2 / 2U;
        const uint64_t candidate_rate =
            (uint64_t)candidate_width * candidate_height *
            presenter->present_fps;
        if (candidate_rate <=
            presenter->target.max_stream_pixels_per_second) {
            break;
        }
        scale_x2--;
    }
    presenter->output_width =
        (uint16_t)((uint32_t)presenter->config.width * scale_x2 / 2U);
    presenter->output_height =
        (uint16_t)((uint32_t)presenter->config.height * scale_x2 / 2U);
    presenter->output_x =
        (uint16_t)((display_width - presenter->output_width) / 2U);
    presenter->output_y =
        (uint16_t)((display_height - presenter->output_height) / 2U);
}

static esp_err_t frame_prepare_mono_output(
    solar_os_frame_presenter_t *presenter)
{
    if (!presenter->mono_fallback) {
        return ESP_OK;
    }
    const uint16_t stride =
        (uint16_t)((presenter->output_width + 7U) / 8U);
    const size_t size = (size_t)stride * presenter->output_height;
    if (size != presenter->mono_size) {
        uint8_t *mono = solar_os_memory_realloc(
            presenter->mono, size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
            "frame.mono");
        if (mono == NULL) {
            return ESP_ERR_NO_MEM;
        }
        presenter->mono = mono;
        presenter->mono_size = size;
    }
    presenter->output_stride = stride;
    return ESP_OK;
}

esp_err_t solar_os_frame_presenter_init(
    solar_os_frame_presenter_t **out,
    const solar_os_frame_presenter_config_t *config)
{
    if (out == NULL || *out != NULL || config == NULL || config->gfx == NULL ||
        config->width == 0U || config->height == 0U ||
        config->stride < frame_minimum_stride(config->format, config->width) ||
        !frame_palette_valid(config) ||
        (uint32_t)config->width > solar_os_gfx_width(config->gfx) ||
        (uint32_t)config->height > solar_os_gfx_height(config->gfx)) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_frame_presenter_t *presenter = solar_os_memory_calloc(
        1U, sizeof(*presenter), SOLAR_OS_MEMORY_INTERNAL_CRITICAL,
        "frame.presenter");
    if (presenter == NULL) {
        return ESP_ERR_NO_MEM;
    }
    presenter->config = *config;
    if (!solar_os_gfx_display_target_name(config->gfx, presenter->target_name,
                                          sizeof(presenter->target_name)) ||
        !solar_os_display_find_target(presenter->target_name,
                                      &presenter->target)) {
        solar_os_memory_free(presenter);
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t format_bit = SOLAR_OS_DISPLAY_FORMAT_BIT(config->format);
    presenter->mono_fallback =
        (presenter->target.frame_formats & format_bit) == 0U;
    if (presenter->mono_fallback &&
        (!config->allow_mono_fallback ||
         config->format == SOLAR_OS_DISPLAY_FORMAT_MONO1 ||
         (presenter->target.frame_formats &
          SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT) == 0U)) {
        solar_os_memory_free(presenter);
        return ESP_ERR_NOT_SUPPORTED;
    }
    frame_prepare_mono_palette(presenter);
    presenter->present_fps = config->preferred_fps != 0U ?
        config->preferred_fps : presenter->target.preferred_stream_fps;
    if (presenter->present_fps == 0U) presenter->present_fps = 25U;
    if (presenter->present_fps > 30U) presenter->present_fps = 30U;
    frame_choose_output(presenter);
    if (presenter->output_width > solar_os_gfx_width(config->gfx) ||
        presenter->output_height > solar_os_gfx_height(config->gfx)) {
        solar_os_memory_free(presenter);
        return ESP_ERR_NOT_SUPPORTED;
    }
    SOLAR_OS_LOGI(TAG,
                  "target=%s format=%u output=%ux%u@%uHz path=%s",
                  presenter->target_name, (unsigned)config->format,
                  (unsigned)presenter->output_width,
                  (unsigned)presenter->output_height,
                  (unsigned)presenter->present_fps,
                  presenter->mono_fallback ? "mono" : "direct");
    presenter->source_size = (size_t)config->height * config->stride;
    presenter->presenting = solar_os_memory_alloc(
        presenter->source_size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "frame.active");
    presenter->queued = solar_os_memory_alloc(
        presenter->source_size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "frame.queued");
    const esp_err_t mono_err = frame_prepare_mono_output(presenter);
    if (presenter->presenting == NULL || presenter->queued == NULL ||
        mono_err != ESP_OK) {
        (void)solar_os_frame_presenter_deinit(presenter);
        return ESP_ERR_NO_MEM;
    }
    memset(presenter->presenting, 0, presenter->source_size);
    memset(presenter->queued, 0, presenter->source_size);
    presenter->mutex = xSemaphoreCreateMutexStatic(&presenter->mutex_storage);
    presenter->requested =
        xSemaphoreCreateBinaryStatic(&presenter->requested_storage);
    if (presenter->mutex == NULL || presenter->requested == NULL) {
        (void)solar_os_frame_presenter_deinit(presenter);
        return ESP_ERR_NO_MEM;
    }
    *out = presenter;
    return solar_os_frame_presenter_resume(presenter);
}

esp_err_t solar_os_frame_presenter_resume(
    solar_os_frame_presenter_t *presenter)
{
    if (presenter == NULL || presenter->mutex == NULL ||
        presenter->requested == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (presenter->task != NULL) {
        return ESP_OK;
    }
    frame_choose_output(presenter);
    if (presenter->output_width > solar_os_gfx_width(presenter->config.gfx) ||
        presenter->output_height > solar_os_gfx_height(presenter->config.gfx)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t mono_err = frame_prepare_mono_output(presenter);
    if (mono_err != ESP_OK) {
        return mono_err;
    }
    if (presenter->config.request_high_refresh) {
        const esp_err_t err = solar_os_display_set_high_refresh_override(
            presenter->target_name, true, FRAME_PRESENTER_HPM_HZ_TENTHS);
        presenter->high_refresh_active = err == ESP_OK;
        if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
            SOLAR_OS_LOGW(TAG, "high refresh unavailable: %s",
                          esp_err_to_name(err));
        }
    }
    while (xSemaphoreTake(presenter->requested, 0) == pdTRUE) {
    }
    if (xSemaphoreTake(presenter->mutex, portMAX_DELAY) == pdTRUE) {
        presenter->stop_requested = false;
        presenter->task_done = false;
        presenter->queued_ready = false;
        xSemaphoreGive(presenter->mutex);
    }
    const BaseType_t created = solar_os_task_create_pinned_internal(
        frame_presenter_worker, "frame_present", FRAME_PRESENTER_STACK,
        presenter, FRAME_PRESENTER_PRIORITY, &presenter->task,
        tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        presenter->task = NULL;
        if (presenter->high_refresh_active) {
            (void)solar_os_display_set_high_refresh_override(
                presenter->target_name, false, 0U);
            presenter->high_refresh_active = false;
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void solar_os_frame_presenter_suspend(solar_os_frame_presenter_t *presenter)
{
    if (presenter == NULL) {
        return;
    }
    TaskHandle_t task = presenter->task;
    if (task != NULL) {
        if (xSemaphoreTake(presenter->mutex, portMAX_DELAY) == pdTRUE) {
            presenter->stop_requested = true;
            xSemaphoreGive(presenter->mutex);
        }
        (void)xSemaphoreGive(presenter->requested);
        if (!solar_os_task_wait_done(task, &presenter->task_done,
                                     SOLAR_OS_TASK_STOP_WAIT_MS)) {
            SOLAR_OS_LOGW(TAG, "present worker stop timed out");
            return;
        }
        presenter->task = NULL;
    }
    if (presenter->high_refresh_active) {
        const esp_err_t err = solar_os_display_set_high_refresh_override(
            presenter->target_name, false, 0U);
        if (err == ESP_OK) {
            presenter->high_refresh_active = false;
        } else {
            SOLAR_OS_LOGW(TAG, "display policy restore failed: %s",
                          esp_err_to_name(err));
        }
    }
}

esp_err_t solar_os_frame_presenter_deinit(solar_os_frame_presenter_t *presenter)
{
    if (presenter == NULL) {
        return ESP_OK;
    }
    solar_os_frame_presenter_suspend(presenter);
    if (presenter->task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (presenter->requested != NULL) {
        vSemaphoreDelete(presenter->requested);
    }
    if (presenter->mutex != NULL) {
        vSemaphoreDelete(presenter->mutex);
    }
    solar_os_memory_free(presenter->mono);
    solar_os_memory_free(presenter->queued);
    solar_os_memory_free(presenter->presenting);
    solar_os_memory_free(presenter);
    return ESP_OK;
}

bool solar_os_frame_presenter_submit(solar_os_frame_presenter_t *presenter,
                                     const uint8_t *data,
                                     size_t data_size)
{
    if (presenter == NULL || data == NULL ||
        data_size < presenter->source_size || presenter->task == NULL ||
        xSemaphoreTake(presenter->mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (presenter->stop_requested) {
        xSemaphoreGive(presenter->mutex);
        return false;
    }
    if (presenter->queued_ready) {
        presenter->stats.replaced_frames++;
    }
    memcpy(presenter->queued, data, presenter->source_size);
    presenter->queued_ready = true;
    xSemaphoreGive(presenter->mutex);
    (void)xSemaphoreGive(presenter->requested);
    return true;
}

void solar_os_frame_presenter_take_stats(
    solar_os_frame_presenter_t *presenter,
    solar_os_frame_presenter_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    if (presenter == NULL ||
        xSemaphoreTake(presenter->mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    *stats = presenter->stats;
    memset(&presenter->stats, 0, sizeof(presenter->stats));
    xSemaphoreGive(presenter->mutex);
}
