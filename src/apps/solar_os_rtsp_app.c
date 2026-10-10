#include "solar_os_rtsp_app.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_audio.h"
#include "solar_os_display.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_media_widgets.h"
#include "solar_os_rtsp_client.h"
#include "solar_os_shell_io.h"
#include "solar_os_signal_widgets.h"
#include "solar_os_stb_image.h"
#include "solar_os_task.h"
#include "solar_os_memory.h"
#include "solar_os_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#define RTSP_NETWORK_STACK 8192U
#define RTSP_DECODE_STACK 24576U
#define RTSP_IMAGE_PIXELS (640U * 480U)
#define RTSP_VIDEO_SLOTS 2U
#define RTSP_HEADER_HEIGHT 28
#define RTSP_CONTROLS_HEIGHT 88
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(RTSP_DECODE_STACK);
#endif

typedef struct {
    uint8_t *pixels;
    uint32_t width, height, timestamp, epoch;
    uint64_t arrived_us;
} rtsp_image_t;

typedef struct {
    solar_os_rtsp_client_t *client;
    solar_os_oscilloscope_widget_t *scope;
    SemaphoreHandle_t image_mutex;
    uint8_t *pixels;
    uint32_t image_width, image_height;
    uint32_t output_width, output_height;
    uint32_t layout_generation;
    rtsp_image_t queue[RTSP_VIDEO_SLOTS];
    unsigned queued;
    bool dirty, diagnostics;
    uint32_t displayed, video_skipped, last_ui_ms;
    uint32_t decoded, decode_us, decode_max_us, scale_us, scale_max_us;
    uint32_t draws, draw_us, draw_max_us, age_max_us, gap_max_us;
    uint32_t blit_us, blit_max_us, present_us, present_max_us;
    uint64_t last_shown_us, last_stats_us;
    uint32_t previous_received, previous_decoded, previous_displayed, previous_draws;
    uint32_t previous_decode_us, previous_scale_us, previous_draw_us, previous_audio_blocks;
    uint32_t previous_blit_us, previous_present_us;
    bool monochrome, fullscreen, frame_diagnostics, direct_rgb565, layout_dirty, direct_started;
    uint64_t last_frame_stats_us;
    uint32_t last_frame_stats_count, fps_tenths;
    bool graphical, suspended, high_refresh, ui_started;
    bool audio_only, stopped, restart;
    int16_t pointer_x, pointer_y;
    solar_os_rtsp_client_status_t last_status;
    char display_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char url[SOLAR_OS_RTSP_URL_MAX];
    TaskHandle_t network_task, decode_task;
    volatile bool network_done, decode_done, stop;
    uint32_t last_port_ms;
    uint32_t decode_errors;
} rtsp_app_state_t;

static void *rtsp_state;
#define rtsp (*(rtsp_app_state_t *)rtsp_state)

static void rtsp_samples(const int16_t *samples, size_t count, uint8_t channels, void *user)
{
    (void)user;
    if (rtsp.scope && channels)
        (void)solar_os_oscilloscope_widget_submit_s16(rtsp.scope, samples, count / channels, channels);
}

static void network_worker(void *arg)
{
    (void)arg;
    (void)solar_os_rtsp_client_run(rtsp.client);
    rtsp.network_done = true;
    for (;;) vTaskSuspend(NULL);
}

/* Scale once on the decoder, not on every UI tick. Integer source maps make
 * the presentation blit unscaled and avoid a 64-bit divide per screen pixel. */
static uint8_t *prepare_image(const uint8_t *source, uint32_t sw, uint32_t sh,
                              uint32_t output_width, uint32_t output_height,
                              uint32_t *dw, uint32_t *dh)
{
    uint32_t w = output_width, h = (uint64_t)sh * w / sw;
    if (h > output_height) { h = output_height; w = (uint64_t)sw * h / sh; }
    if (!w || !h || w > RTSP_IMAGE_PIXELS / h) return NULL;
    const unsigned channels = rtsp.monochrome ? 1U : 3U;
    const size_t stride = (size_t)w * channels;
    uint8_t *pixels = solar_os_memory_alloc(stride * h,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "rtsp.image");
    if (!pixels) return NULL;
    uint32_t previous_sy = UINT32_MAX;
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t sy = (uint64_t)y * sh / h;
        uint8_t *dst = pixels + (size_t)y * stride;
        /* Upscaling repeats source rows; copy the already scaled row. */
        if (sy == previous_sy) { memcpy(dst, dst - stride, stride); continue; }
        previous_sy = sy;
        const uint8_t *row = source + (size_t)sy * sw * channels;
        uint32_t sx = 0, remainder = 0;
        for (uint32_t x = 0; x < w; x++) {
            if (channels == 1U) dst[x] = row[sx];
            else {
                dst[x * 3U] = row[sx * 3U];
                dst[x * 3U + 1U] = row[sx * 3U + 1U];
                dst[x * 3U + 2U] = row[sx * 3U + 2U];
            }
            remainder += sw;
            while (remainder >= w) { sx++; remainder -= w; }
        }
    }
    *dw = w; *dh = h; return pixels;
}

static void decode_worker(void *arg)
{
    (void)arg;
    while (!rtsp.stop && !rtsp.network_done) {
        if (rtsp.suspended) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
        bool full = rtsp.queued == RTSP_VIDEO_SLOTS;
        uint32_t output_width = rtsp.output_width, output_height = rtsp.output_height;
        uint32_t generation = rtsp.layout_generation;
        xSemaphoreGive(rtsp.image_mutex);
        /* Preserve frames waiting for their presentation deadline. Replacing
         * the oldest on each fast source tick would starve playback forever. */
        if (full) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        solar_os_rtp_jpeg_frame_t frame;
        solar_os_rtsp_client_status_t status;
        solar_os_rtsp_client_status(rtsp.client, &status);
        uint64_t arrived;
        if (!solar_os_rtsp_client_take_video(rtsp.client, &frame, &arrived)) {
            vTaskDelay(pdMS_TO_TICKS(5)); continue;
        }
        uint8_t *pixels = NULL; uint32_t w = 0, h = 0;
        uint64_t decode_start = rtsp.diagnostics ? esp_timer_get_time() : 0;
        esp_err_t err = rtsp.monochrome ?
            solar_os_stb_jpeg_decode_gray(frame.data, frame.length,
                RTSP_IMAGE_PIXELS, &pixels, &w, &h) :
            rtsp.direct_rgb565 ?
            solar_os_stb_decode_jpeg_rgb565_scaled(frame.data, frame.length,
                RTSP_IMAGE_PIXELS, output_width, output_height, &pixels, &w, &h) :
            solar_os_stb_decode_jpeg_rgb_scaled(frame.data, frame.length,
                RTSP_IMAGE_PIXELS, output_width, output_height, &pixels, &w, &h);
        solar_os_rtsp_client_release_video(rtsp.client);
        if (err != ESP_OK) {
            xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY); rtsp.decode_errors++;
            xSemaphoreGive(rtsp.image_mutex); continue;
        }
        uint64_t scale_start = rtsp.diagnostics ? esp_timer_get_time() : 0;
        uint32_t draw_w = w, draw_h = h;
        uint8_t *prepared;
        if (rtsp.direct_rgb565) {
            /* Retain the compact decoded raster, not an enlarged RGB888 copy.
             * The display driver scales wire-order RGB565 into DMA bands. */
            prepared = pixels;
        } else {
            prepared = prepare_image(pixels, w, h, output_width, output_height, &draw_w, &draw_h);
            solar_os_stb_image_free(pixels);
        }
        if (!prepared) {
            xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY); rtsp.decode_errors++;
            xSemaphoreGive(rtsp.image_mutex); continue;
        }
        uint64_t prepared_us = rtsp.diagnostics ? esp_timer_get_time() : 0;
        xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
        if (rtsp.diagnostics) {
            uint32_t decode = scale_start - decode_start, scale = prepared_us - scale_start;
            rtsp.decoded++; rtsp.decode_us += decode; rtsp.scale_us += scale;
            if (decode > rtsp.decode_max_us) rtsp.decode_max_us = decode;
            if (scale > rtsp.scale_max_us) rtsp.scale_max_us = scale;
        }
        if (generation != rtsp.layout_generation) {
            /* F may change the layout while decoding. Never queue a frame
             * prepared for the previous viewport after the UI cleared it. */
            rtsp.video_skipped++;
            xSemaphoreGive(rtsp.image_mutex);
            solar_os_memory_free(prepared);
            continue;
        }
        /* Only this worker adds entries; the UI can only remove them. */
        rtsp.queue[rtsp.queued++] = (rtsp_image_t){.pixels = prepared,
            .width = draw_w, .height = draw_h, .timestamp = frame.timestamp,
            .epoch = status.epoch, .arrived_us = arrived};
        xSemaphoreGive(rtsp.image_mutex);
    }
    rtsp.decode_done = true;
    for (;;) vTaskSuspend(NULL);
}

static void refresh_override(solar_os_context_t *ctx, bool enabled)
{
    if (!rtsp.graphical || rtsp.high_refresh == enabled) return;
    if (enabled && !solar_os_gfx_display_target_name(solar_os_context_gfx(ctx),
        rtsp.display_target, sizeof(rtsp.display_target))) return;
    if (solar_os_display_set_high_refresh_override(rtsp.display_target, enabled, 255U) == ESP_OK)
        rtsp.high_refresh = enabled;
}

static void playback_status(solar_os_rtsp_client_status_t *status)
{
    if (rtsp.client) solar_os_rtsp_client_status(rtsp.client, &rtsp.last_status);
    *status = rtsp.last_status;
    if (rtsp.stopped) status->playing = status->audio_playing = false;
}

static const char *playback_label(const solar_os_rtsp_client_status_t *status)
{
    if (rtsp.stopped) return rtsp.network_done && rtsp.decode_done ? "STOPPED" : "STOPPING";
    if (status->reconnecting) return "RECONNECTING";
    if (!status->playing) return "CONNECTING";
    if (status->audio && !status->audio_playing) return "BUFFERING";
    return "PLAYING";
}

static void centered_text(solar_os_gfx_t *gfx, int width, int baseline, char *text)
{
    while (text[0] && solar_os_gfx_text_width(gfx, text) > (size_t)(width - 14))
        text[strlen(text) - 1] = 0;
    solar_os_gfx_text(gfx, (width - (int)solar_os_gfx_text_width(gfx, text)) / 2, baseline, text);
}

static void draw_controls(solar_os_gfx_t *gfx, int w, int h,
                          const solar_os_rtsp_client_status_t *status)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, w, RTSP_HEADER_HEIGHT);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    solar_os_gfx_text(gfx, 7, 19, "RTSP");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    const char *label = playback_label(status);
    solar_os_gfx_text(gfx, w - (int)solar_os_gfx_text_width(gfx, label) - 7, 18, label);
    solar_os_gfx_fill_rect(gfx, 0, h - RTSP_CONTROLS_HEIGHT, w, RTSP_CONTROLS_HEIGHT);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, 0, h - RTSP_CONTROLS_HEIGHT, w - 1, h - RTSP_CONTROLS_HEIGHT);
    char title[SOLAR_OS_RTSP_URL_MAX];
    if (solar_os_rtsp_url_redact(rtsp.url, title, sizeof(title)) != ESP_OK)
        snprintf(title, sizeof(title), "%s", rtsp.url + strlen("rtsp://"));
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    centered_text(gfx, w, h - 68, title);
    char detail[96];
    if (status->video && status->audio)
        snprintf(detail, sizeof(detail), "JPEG %lu.%lu fps  L16 %lu Hz %u ch",
            (unsigned long)(rtsp.fps_tenths / 10), (unsigned long)(rtsp.fps_tenths % 10),
            (unsigned long)status->sample_rate, status->channels);
    else if (status->video)
        snprintf(detail, sizeof(detail), "JPEG %lu.%lu fps",
            (unsigned long)(rtsp.fps_tenths / 10), (unsigned long)(rtsp.fps_tenths % 10));
    else if (status->audio)
        snprintf(detail, sizeof(detail), "L16 %lu Hz  %u ch",
            (unsigned long)status->sample_rate, status->channels);
    else snprintf(detail, sizeof(detail), "%s", label);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    centered_text(gfx, w, h - 52, detail);
    solar_os_audio_status_t audio;
    solar_os_audio_get_status(&audio);
    const int volume_width = w / 2, volume_x = (w - volume_width) / 2, volume_y = h - 44;
    solar_os_gfx_text(gfx, volume_x - 29, volume_y + 9, "VOL");
    solar_os_gfx_rect(gfx, volume_x, volume_y, volume_width, 10);
    if (audio.volume)
        solar_os_gfx_fill_rect(gfx, volume_x + 2, volume_y + 2,
            (volume_width - 4) * audio.volume / 100, 6);
    char percent[8];
    snprintf(percent, sizeof(percent), "%u%%", audio.volume);
    solar_os_gfx_text(gfx, volume_x + volume_width + 5, volume_y + 9, percent);
    /* A URL is a single live source, not a playlist. Keep the common center
     * button without offering non-functional previous/next controls. */
    const int button_width = (w - 20) / 3;
    solar_os_media_transport_button_draw(gfx, (w - button_width) / 2, h - 25, button_width, 21,
        rtsp.stopped ? SOLAR_OS_MEDIA_TRANSPORT_PLAY : SOLAR_OS_MEDIA_TRANSPORT_STOP, false);
}

static void render(solar_os_context_t *ctx, bool force)
{
    if (!rtsp.graphical || rtsp.suspended) return;
    solar_os_rtsp_client_status_t status;
    playback_status(&status);
    bool changed = false;
    uint64_t arrived = 0;
    xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
    while (rtsp.queued) {
        rtsp_image_t image = rtsp.queue[0];
        if (image.epoch != status.epoch || !status.playing) {
            memmove(rtsp.queue, rtsp.queue + 1, (--rtsp.queued) * sizeof(rtsp.queue[0]));
            solar_os_memory_free(image.pixels); rtsp.video_skipped++; continue;
        }
        int64_t late = solar_os_rtsp_client_video_lateness(rtsp.client, image.timestamp, image.arrived_us);
        if (late < 0) break;
        memmove(rtsp.queue, rtsp.queue + 1, (--rtsp.queued) * sizeof(rtsp.queue[0]));
        if (late > 150000) { solar_os_memory_free(image.pixels); rtsp.video_skipped++; continue; }
        if (changed) rtsp.video_skipped++; /* Due frames coalesced into one draw. */
        solar_os_memory_free(rtsp.pixels);
        rtsp.pixels = image.pixels; rtsp.image_width = image.width; rtsp.image_height = image.height;
        arrived = image.arrived_us; changed = true;
    }
    bool dirty = rtsp.dirty;
    rtsp.dirty = false;
    xSemaphoreGive(rtsp.image_mutex);
    if ((status.video || rtsp.stopped) && !changed && !dirty && !force) return;
    uint64_t draw_start = rtsp.diagnostics ? esp_timer_get_time() : 0;
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    int w = solar_os_gfx_width(gfx), h = solar_os_gfx_height(gfx);
    const bool direct = rtsp.direct_rgb565 && status.video && !rtsp.stopped;
    if (direct && !rtsp.direct_started) { rtsp.layout_dirty = true; rtsp.direct_started = true; }
    const bool chrome = !direct || dirty || force || rtsp.layout_dirty;
    if (!direct || rtsp.layout_dirty) solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    rtsp.layout_dirty = false;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    if (chrome && !rtsp.fullscreen) draw_controls(gfx, w, h, &status);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    int content_top = rtsp.fullscreen ? 0 : RTSP_HEADER_HEIGHT;
    int content_height = rtsp.fullscreen ? h : h - RTSP_HEADER_HEIGHT - RTSP_CONTROLS_HEIGHT;
    const int diagnostics_top = content_top;
    if (direct && rtsp.frame_diagnostics) { content_top += 16; content_height -= 16; }
    if (rtsp.stopped) {
        solar_os_gfx_text(gfx, 7, content_top + 20, rtsp.restart ? "Reconnecting..." : "Enter / tap Play to reconnect");
    } else if (status.video && !direct) {
        xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
        if (rtsp.pixels) {
            const unsigned channels = rtsp.monochrome ? 1U : 3U;
            const solar_os_gfx_raster_t raster = {
                .pixels = rtsp.pixels, .pixels_size = rtsp.image_width * rtsp.image_height * channels,
                .width = rtsp.image_width, .height = rtsp.image_height,
                .stride = rtsp.image_width * channels,
                .format = rtsp.monochrome ? SOLAR_OS_GFX_RASTER_GRAY8 : SOLAR_OS_GFX_RASTER_RGB888,
            };
            int draw_w = rtsp.image_width, draw_h = rtsp.image_height;
            uint64_t blit_start = rtsp.diagnostics ? esp_timer_get_time() : 0;
            (void)solar_os_gfx_blit_raster(gfx, &raster, (w - draw_w) / 2,
                content_top + (content_height - draw_h) / 2, draw_w, draw_h, NULL);
            if (rtsp.diagnostics) {
                uint32_t duration = esp_timer_get_time() - blit_start;
                rtsp.blit_us += duration;
                if (duration > rtsp.blit_max_us) rtsp.blit_max_us = duration;
            }
        } else solar_os_gfx_text(gfx, 5, 40, "Waiting for JPEG video...");
        xSemaphoreGive(rtsp.image_mutex);
    } else if (!status.video && status.audio) {
        const int margin = rtsp.fullscreen ? 0 : 3;
        solar_os_oscilloscope_widget_draw(rtsp.scope, gfx, margin, content_top + margin,
            w - 2 * margin, content_height - 2 * margin);
    } else if (!status.video && chrome) solar_os_gfx_text(gfx, 5, 40, "Connecting...");
    if (chrome && rtsp.frame_diagnostics && status.video && !rtsp.stopped) {
        char frames[96];
        snprintf(frames, sizeof(frames), "RX %lu  SHOWN %lu  DROP %lu/%lu  %lu.%lu fps",
            (unsigned long)status.video_frames, (unsigned long)rtsp.displayed,
            (unsigned long)status.video_dropped, (unsigned long)rtsp.video_skipped,
            (unsigned long)(rtsp.fps_tenths / 10), (unsigned long)(rtsp.fps_tenths % 10));
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_fill_rect(gfx, 0, diagnostics_top, w, 16);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_text(gfx, 5, diagnostics_top + 12, frames);
    }
    uint64_t present_start = rtsp.diagnostics ? esp_timer_get_time() : 0;
    if (chrome) solar_os_gfx_present(gfx);
    if (direct) {
        xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
        if (rtsp.pixels) {
            uint32_t dw = w, dh = (uint64_t)rtsp.image_height * dw / rtsp.image_width;
            if (dh > (uint32_t)content_height) {
                dh = content_height; dw = (uint64_t)rtsp.image_width * dh / rtsp.image_height;
            }
            const solar_os_display_raster_t frame = {
                .data = rtsp.pixels, .data_size = (size_t)rtsp.image_width * rtsp.image_height * 2U,
                .source_width = rtsp.image_width, .source_height = rtsp.image_height,
                .source_stride = rtsp.image_width * 2U,
                .x = (w - dw) / 2, .y = content_top + (content_height - dh) / 2,
                .width = dw, .height = dh, .format = SOLAR_OS_DISPLAY_FORMAT_RGB565,
            };
            esp_err_t err = solar_os_gfx_present_frame(gfx, &frame);
            if (err != ESP_OK) {
                xSemaphoreGive(rtsp.image_mutex);
                solar_os_context_finish(ctx, 1, err == ESP_ERR_NO_MEM ?
                    "RTSP: RGB565 display rotation buffer allocation failed (PSRAM)" :
                    "RTSP: RGB565 display frame/transfer failed; check display driver log");
                return;
            }
        }
        xSemaphoreGive(rtsp.image_mutex);
    }
    if (changed) rtsp.displayed++;
    if (rtsp.diagnostics) {
        uint64_t now = esp_timer_get_time();
        uint32_t present = now - present_start;
        rtsp.present_us += present;
        if (present > rtsp.present_max_us) rtsp.present_max_us = present;
        uint32_t duration = now - draw_start;
        rtsp.draws++; rtsp.draw_us += duration;
        if (duration > rtsp.draw_max_us) rtsp.draw_max_us = duration;
        if (changed) {
            uint32_t age = now - arrived;
            uint32_t gap = rtsp.last_shown_us ? now - rtsp.last_shown_us : 0;
            if (age > rtsp.age_max_us) rtsp.age_max_us = age;
            if (gap > rtsp.gap_max_us) rtsp.gap_max_us = gap;
            rtsp.last_shown_us = now;
        }
    }
}

static void diagnostics_tick(const solar_os_rtsp_client_status_t *status)
{
    if (!rtsp.diagnostics) return;
    uint64_t now = esp_timer_get_time();
    if (now - rtsp.last_stats_us < 1000000) return;
    uint32_t window_ms = (now - rtsp.last_stats_us) / 1000;
    rtsp.last_stats_us = now;
    if (rtsp.image_mutex) xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
    uint32_t decoded = rtsp.decoded - rtsp.previous_decoded;
    uint32_t draws = rtsp.draws - rtsp.previous_draws;
    uint32_t decode_avg = decoded ? (rtsp.decode_us - rtsp.previous_decode_us) / decoded : 0;
    uint32_t scale_avg = decoded ? (rtsp.scale_us - rtsp.previous_scale_us) / decoded : 0;
    uint32_t draw_avg = draws ? (rtsp.draw_us - rtsp.previous_draw_us) / draws : 0;
    uint32_t decode_max = rtsp.decode_max_us, scale_max = rtsp.scale_max_us;
    unsigned queued = rtsp.queued;
    uint32_t errors = rtsp.decode_errors;
    rtsp.previous_decoded = rtsp.decoded;
    rtsp.previous_decode_us = rtsp.decode_us; rtsp.previous_scale_us = rtsp.scale_us;
    if (rtsp.image_mutex) xSemaphoreGive(rtsp.image_mutex);
    SOLAR_OS_LOGI("rtsp.stats", "video %lums rx=%lu dec=%lu shown=%lu q=%u drops=%lu/%lu errors=%lu age_max=%luus gap_max=%luus",
        (unsigned long)window_ms, (unsigned long)(status->video_frames - rtsp.previous_received), (unsigned long)decoded,
        (unsigned long)(rtsp.displayed - rtsp.previous_displayed), queued,
        (unsigned long)status->video_dropped, (unsigned long)rtsp.video_skipped, (unsigned long)errors,
        (unsigned long)rtsp.age_max_us, (unsigned long)rtsp.gap_max_us);
    SOLAR_OS_LOGI("rtsp.stats", "us avg/max decode=%lu/%lu scale=%lu/%lu draw=%lu/%lu",
        (unsigned long)decode_avg, (unsigned long)decode_max, (unsigned long)scale_avg,
        (unsigned long)scale_max, (unsigned long)draw_avg, (unsigned long)rtsp.draw_max_us);
    SOLAR_OS_LOGI("rtsp.stats", "us avg/max blit=%lu/%lu present=%lu/%lu",
        (unsigned long)(draws ? (rtsp.blit_us - rtsp.previous_blit_us) / draws : 0),
        (unsigned long)rtsp.blit_max_us,
        (unsigned long)(draws ? (rtsp.present_us - rtsp.previous_present_us) / draws : 0),
        (unsigned long)rtsp.present_max_us);
    SOLAR_OS_LOGI("rtsp.stats", "audio %luHz/%u -> %luHz/%u quantum=%u blocks/s=%lu q=%lu drop=%lu conceal=%lu rebuffer=%lu",
        (unsigned long)status->sample_rate, status->channels, (unsigned long)status->audio_output_rate,
        status->audio_output_channels, status->audio_block_frames,
        (unsigned long)(status->audio_blocks - rtsp.previous_audio_blocks),
        (unsigned long)status->audio_queued, (unsigned long)status->audio_dropped, (unsigned long)status->audio_concealed,
        (unsigned long)status->audio_rebuffers);
    SOLAR_OS_LOGI("rtsp.stats", "audio us write_max=%lu submit_gap_max=%lu jitter_wait_polls=%lu output_frames=%lu silence_frames=%lu",
        (unsigned long)status->audio_write_max_us, (unsigned long)status->audio_gap_max_us,
        (unsigned long)status->audio_wait_polls, (unsigned long)status->audio_output_frames,
        (unsigned long)status->audio_concealed_frames);
    SOLAR_OS_LOGI("rtsp.stats", "stack free bytes net=%lu/%u jpeg=%lu/%u sink=%lu/8192 epoch=%lu retries=%lu",
        (unsigned long)(rtsp.network_task ? uxTaskGetStackHighWaterMark(rtsp.network_task) : 0), RTSP_NETWORK_STACK,
        (unsigned long)(rtsp.decode_task ? uxTaskGetStackHighWaterMark(rtsp.decode_task) : 0), RTSP_DECODE_STACK,
        (unsigned long)status->audio_stack_min_free, (unsigned long)status->epoch, (unsigned long)status->reconnects);
    SOLAR_OS_LOGI("rtsp.stats", "heap bytes internal=%u largest=%u dma=%u low=%u largest=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    rtsp.previous_received = status->video_frames; rtsp.previous_displayed = rtsp.displayed;
    rtsp.previous_draws = rtsp.draws; rtsp.previous_draw_us = rtsp.draw_us;
    rtsp.previous_blit_us = rtsp.blit_us; rtsp.previous_present_us = rtsp.present_us;
    rtsp.previous_audio_blocks = status->audio_blocks;
}

static esp_err_t start_playback(solar_os_context_t *ctx)
{
    const solar_os_rtsp_client_options_t options = {
        .video = rtsp.graphical && !rtsp.audio_only, .audio = true, .samples = rtsp_samples,
        .diagnostics = rtsp.diagnostics,
        .reconnect_attempts = 6,
    };
    rtsp.stop = false;
    rtsp.network_done = rtsp.decode_done = true;
    rtsp.last_status = (solar_os_rtsp_client_status_t){0};
    esp_err_t err = solar_os_rtsp_client_create(rtsp.url, &options, &rtsp.client);
    if (err != ESP_OK) {
        solar_os_context_finish(ctx, 1, err == ESP_ERR_NO_MEM ?
            "RTSP: client allocation failed" :
            "RTSP: malformed/unsupported URL; use rtsp://[user[:pass]@]host[:port]/path (no IPv6)");
        return err;
    }
    rtsp.network_done = false;
    if (solar_os_task_create_pinned_external(network_worker, "rtsp-net", RTSP_NETWORK_STACK, NULL,
        tskIDLE_PRIORITY + 2, &rtsp.network_task, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        rtsp.network_done = true;
        solar_os_context_finish(ctx, 1, "RTSP: network worker could not start (stack/admission)");
        return ESP_ERR_NO_MEM;
    }
    rtsp.stopped = false;
    rtsp.dirty = rtsp.layout_dirty = true;
    rtsp.previous_received = rtsp.previous_audio_blocks = 0;
    rtsp.last_stats_us = esp_timer_get_time();
    rtsp.last_frame_stats_us = 0;
    rtsp.fps_tenths = 0;
    if (rtsp.graphical && !rtsp.suspended) refresh_override(ctx, true);
    return ESP_OK;
}

/* The network worker owns the audio sink and client. Never destroy them or
 * reuse the stop flag until BOTH workers have finished their current run. */
static void reap_playback(void)
{
    if (!rtsp.network_done || !rtsp.decode_done) return;
    if (rtsp.network_task) {
        solar_os_task_delete_external(rtsp.network_task);
        rtsp.network_task = NULL;
    }
    if (rtsp.decode_task) {
        solar_os_task_delete_external(rtsp.decode_task);
        rtsp.decode_task = NULL;
    }
    if (rtsp.client && solar_os_rtsp_client_destroy(rtsp.client) == ESP_OK) {
        rtsp.client = NULL;
        rtsp.dirty = true;
    }
}

static void stop_playback(solar_os_context_t *ctx)
{
    rtsp.stopped = true;
    rtsp.restart = false;
    rtsp.stop = true;
    solar_os_rtsp_client_cancel(rtsp.client);
    refresh_override(ctx, false);
    if (rtsp.image_mutex) xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
    rtsp.layout_generation++;
    solar_os_memory_free(rtsp.pixels); rtsp.pixels = NULL;
    for (unsigned i = 0; i < rtsp.queued; i++) solar_os_memory_free(rtsp.queue[i].pixels);
    rtsp.queued = 0;
    rtsp.dirty = rtsp.layout_dirty = true;
    rtsp.direct_started = false;
    if (rtsp.image_mutex) xSemaphoreGive(rtsp.image_mutex);
}

static void toggle_playback(solar_os_context_t *ctx)
{
    if (rtsp.stopped) rtsp.restart = true;
    else stop_playback(ctx);
    rtsp.dirty = true;
}

static esp_err_t start(solar_os_context_t *ctx)
{
    rtsp.network_done = rtsp.decode_done = true;
    const char *url = NULL;
    for (int i = 1; i < solar_os_context_argc(ctx); i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (!strcmp(arg, "--audio-only")) rtsp.audio_only = true;
        else if (!strcmp(arg, "--stats")) rtsp.diagnostics = true;
        else if (!url) url = arg;
        else return ESP_ERR_INVALID_ARG;
    }
    if (!url) return ESP_ERR_INVALID_ARG;
    esp_err_t err = solar_os_rtsp_url_normalize(url, rtsp.url, sizeof(rtsp.url));
    if (err != ESP_OK) {
        solar_os_context_finish(ctx, 1,
            "RTSP: malformed/unsupported address; use [rtsp://][user[:pass]@]host[:port][/path] (no IPv6)");
        return err;
    }
    rtsp.frame_diagnostics = rtsp.diagnostics;
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    rtsp.graphical = solar_os_context_gfx(ctx) &&
        (!io || solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_PORT);
    solar_os_context_set_app_class(ctx, rtsp.graphical ? SOLAR_OS_APP_CLASS_GUI : SOLAR_OS_APP_CLASS_COMMAND);
    if (rtsp.graphical) {
        esp_err_t err = solar_os_oscilloscope_widget_create(256, &rtsp.scope);
        if (err != ESP_OK) return err;
        rtsp.image_mutex = xSemaphoreCreateMutex();
        if (!rtsp.image_mutex) return ESP_ERR_NO_MEM;
        /* Activate the target's color surface BEFORE selecting the decoder.
         * A fresh/suspended context otherwise still reports MONO1. */
        rtsp.ui_started = true;
        solar_os_context_set_graphics_active(ctx, true);
        rtsp.output_width = solar_os_gfx_width(solar_os_context_gfx(ctx));
        rtsp.monochrome = solar_os_gfx_format(solar_os_context_gfx(ctx)) == SOLAR_OS_DISPLAY_FORMAT_MONO1;
        rtsp.direct_rgb565 = !rtsp.monochrome && solar_os_gfx_supports_frame_format(
            solar_os_context_gfx(ctx), SOLAR_OS_DISPLAY_FORMAT_RGB565);
        rtsp.layout_dirty = true;
        int body_height = solar_os_gfx_height(solar_os_context_gfx(ctx)) - RTSP_HEADER_HEIGHT - RTSP_CONTROLS_HEIGHT;
        if (body_height <= 0) return ESP_ERR_NOT_SUPPORTED;
        rtsp.output_height = body_height - (rtsp.direct_rgb565 && rtsp.frame_diagnostics ? 16 : 0);
        SOLAR_OS_LOGI("rtsp", "video output %s viewport=%lux%lu",
            rtsp.monochrome ? "GRAY8" : rtsp.direct_rgb565 ? "RGB565 direct" : "RGB888",
            (unsigned long)rtsp.output_width, (unsigned long)rtsp.output_height);
    }
    err = start_playback(ctx);
    if (err != ESP_OK) return err;
    rtsp.ui_started = true;
    if (!rtsp.graphical) solar_os_shell_io_printf(io, "RTSP %s (audio only on port shell)\r\n", rtsp.url);
    render(ctx, true);
    return ESP_OK;
}

static void stop(solar_os_context_t *ctx)
{
    rtsp.stop = true;
    solar_os_rtsp_client_cancel(rtsp.client);
    if (rtsp.ui_started && rtsp.graphical) { refresh_override(ctx, false); solar_os_context_set_graphics_active(ctx, false); }
    rtsp.ui_started = false;
    /* DNS and device writes have their own deadlines. If they outlive this
     * shell stop budget, retain cold state until the owners have finished. */
    (void)solar_os_task_wait_done(rtsp.network_task, &rtsp.network_done, SOLAR_OS_TASK_STOP_WAIT_MS);
    (void)solar_os_task_wait_done(rtsp.decode_task, &rtsp.decode_done, SOLAR_OS_TASK_STOP_WAIT_MS);
}

static bool release_ready(void) { return rtsp.network_done && rtsp.decode_done; }

static void cleanup(void)
{
    reap_playback();
    solar_os_oscilloscope_widget_destroy(rtsp.scope);
    solar_os_memory_free(rtsp.pixels);
    for (unsigned i = 0; i < rtsp.queued; i++) solar_os_memory_free(rtsp.queue[i].pixels);
    if (rtsp.image_mutex) vSemaphoreDelete(rtsp.image_mutex);
}

static void suspend(solar_os_context_t *ctx)
{
    rtsp.suspended = true; refresh_override(ctx, false);
    solar_os_context_set_graphics_active(ctx, false);
}
static void resume(solar_os_context_t *ctx)
{
    rtsp.layout_dirty = true;
    rtsp.suspended = false; refresh_override(ctx, !rtsp.stopped);
    solar_os_context_set_graphics_active(ctx, rtsp.graphical); render(ctx, true);
}

static bool pointer(solar_os_context_t *ctx, const solar_os_input_pointer_event_t *event)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (!rtsp.graphical || !gfx) return false;
    int w = solar_os_gfx_width(gfx), h = solar_os_gfx_height(gfx);
    if (event->mode == SOLAR_OS_INPUT_POINTER_ABSOLUTE) {
        rtsp.pointer_x = event->x;
        rtsp.pointer_y = event->y;
    } else {
        int x = rtsp.pointer_x + event->delta_x, y = rtsp.pointer_y + event->delta_y;
        rtsp.pointer_x = x < 0 ? 0 : x >= w ? w - 1 : x;
        rtsp.pointer_y = y < 0 ? 0 : y >= h ? h - 1 : y;
    }
    int width = (w - 20) / 3, left = (w - width) / 2;
    if (rtsp.fullscreen || event->action != SOLAR_OS_INPUT_POINTER_PRESS ||
        !(event->buttons & SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY) ||
        rtsp.pointer_y < h - 25 || rtsp.pointer_y >= h - 4 ||
        rtsp.pointer_x < left || rtsp.pointer_x >= left + width)
        return false;
    toggle_playback(ctx);
    return true;
}

static bool event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event->type == SOLAR_OS_EVENT_RESUME) { resume(ctx); return true; }
    if (event->type == SOLAR_OS_EVENT_POINTER) return pointer(ctx, &event->data.pointer);
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        uint8_t key = event->data.ch;
        if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE || key == 'q' || key == 'Q')
            solar_os_context_finish(ctx, 0, NULL);
        else if (key == '\r' || key == '\n' || key == SOLAR_OS_KEY_ENTER || key == ' ')
            toggle_playback(ctx);
        else if (key == SOLAR_OS_KEY_UP || key == SOLAR_OS_KEY_DOWN || key == '+' || key == '-') {
            solar_os_audio_status_t status; solar_os_audio_get_status(&status);
            int volume = status.volume + (key == SOLAR_OS_KEY_UP || key == '+' ? 5 : -5);
            (void)solar_os_audio_set_volume(volume < 0 ? 0 : volume > 100 ? 100 : volume);
            rtsp.dirty = true;
        } else if (rtsp.graphical && (key == 'f' || key == 'F')) {
            xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
            rtsp.fullscreen = !rtsp.fullscreen;
            rtsp.output_height = solar_os_gfx_height(solar_os_context_gfx(ctx)) -
                (rtsp.fullscreen ? 0 : RTSP_HEADER_HEIGHT + RTSP_CONTROLS_HEIGHT) -
                (rtsp.direct_rgb565 && rtsp.frame_diagnostics ? 16 : 0);
            rtsp.layout_generation++;
            solar_os_memory_free(rtsp.pixels); rtsp.pixels = NULL;
            for (unsigned i = 0; i < rtsp.queued; i++) solar_os_memory_free(rtsp.queue[i].pixels);
            rtsp.queued = 0; rtsp.dirty = rtsp.layout_dirty = true;
            xSemaphoreGive(rtsp.image_mutex);
            if (rtsp.diagnostics) SOLAR_OS_LOGI("rtsp", "fullscreen=%u viewport=%lux%lu",
                rtsp.fullscreen, (unsigned long)rtsp.output_width, (unsigned long)rtsp.output_height);
        } else if (key == 'd' || key == 'D') {
            rtsp.frame_diagnostics = !rtsp.frame_diagnostics;
            rtsp.dirty = rtsp.layout_dirty = true;
            if (rtsp.direct_rgb565) {
                xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
                rtsp.output_height = solar_os_gfx_height(solar_os_context_gfx(ctx)) -
                    (rtsp.fullscreen ? 0 : RTSP_HEADER_HEIGHT + RTSP_CONTROLS_HEIGHT) -
                    (rtsp.frame_diagnostics ? 16 : 0);
                rtsp.layout_generation++;
                solar_os_memory_free(rtsp.pixels); rtsp.pixels = NULL;
                for (unsigned i = 0; i < rtsp.queued; i++) solar_os_memory_free(rtsp.queue[i].pixels);
                rtsp.queued = 0;
                xSemaphoreGive(rtsp.image_mutex);
            }
            if (rtsp.diagnostics) SOLAR_OS_LOGI("rtsp", "frame diagnostics=%u", rtsp.frame_diagnostics);
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_TICK) return false;
    if (rtsp.stopped) {
        reap_playback();
        if (rtsp.restart && !rtsp.client) {
            rtsp.restart = false;
            if (start_playback(ctx) != ESP_OK) return true;
        } else {
            render(ctx, false);
            return true;
        }
    }
    solar_os_rtsp_client_status_t status;
    playback_status(&status);
    if (rtsp.network_done) {
        char message[128];
        snprintf(message, sizeof(message), "RTSP: %s", status.error_detail[0] ?
            status.error_detail : esp_err_to_name(status.error));
        solar_os_context_finish(ctx, status.error == ESP_OK ? 0 : 1, status.error == ESP_OK ? NULL : message);
        return true;
    }
    if (status.video && !rtsp.decode_task) {
        rtsp.decode_done = false;
        if (solar_os_task_create_pinned_external(decode_worker, "rtsp-jpeg", RTSP_DECODE_STACK, NULL,
            tskIDLE_PRIORITY + 1, &rtsp.decode_task, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
            rtsp.decode_done = true;
            solar_os_context_finish(ctx, 1, "RTSP JPEG decoder task unavailable"); return true;
        }
    }
    if (!rtsp.graphical && event->data.tick_ms - rtsp.last_port_ms >= 1000) {
        rtsp.last_port_ms = event->data.tick_ms;
        solar_os_shell_io_printf(solar_os_context_shell_io(ctx), "%s  %lu Hz  %u ch  dropped %lu\r\n",
            status.audio_playing ? "Playing" : "Connecting/buffering", (unsigned long)status.sample_rate,
            status.channels, (unsigned long)status.audio_dropped);
    }
    bool refresh = event->data.tick_ms - rtsp.last_ui_ms >= 1000;
    if (refresh) rtsp.last_ui_ms = event->data.tick_ms;
    uint64_t now = esp_timer_get_time();
    if (now - rtsp.last_frame_stats_us >= 1000000) {
        if (rtsp.last_frame_stats_us)
            rtsp.fps_tenths = (uint64_t)(rtsp.displayed - rtsp.last_frame_stats_count) * 10000000 /
                (now - rtsp.last_frame_stats_us);
        rtsp.last_frame_stats_us = now; rtsp.last_frame_stats_count = rtsp.displayed;
    }
    render(ctx, refresh); diagnostics_tick(&status); return true;
}

const solar_os_app_t solar_os_rtsp_app = {
    .name = "rtsp", .summary = "RTSP JPEG/L16 viewer", .app_class = SOLAR_OS_APP_CLASS_GUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE | SOLAR_OS_APP_FLAG_POINTER_EVENTS,
    .start = start, .stop = stop, .suspend = suspend, .resume = resume, .event = event,
    .state_slot = &rtsp_state, .state_size = sizeof(rtsp_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .state_release_ready = release_ready, .state_release_cleanup = cleanup,
    .tick_interval_ms = 20, .worker_stack_bytes = RTSP_NETWORK_STACK, .worker_stack_external = true,
};
