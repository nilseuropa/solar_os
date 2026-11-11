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
#include "solar_os_media_preferences.h"
#include "solar_os_media_playlist.h"
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
    bool audio_only, stopped, restart, paused;
    bool playlist_visible, playlist_dirty;
    solar_os_media_playlist_t playlist;
    solar_os_media_preferences_t *preferences;
    size_t active, cursor, top;
    unsigned dialog;
    size_t edit_index, dialog_input_len;
    char dialog_name[SOLAR_OS_M3U_TITLE_MAX], dialog_input[SOLAR_OS_RTSP_URL_MAX];
    char message[96];
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
        if (rtsp.suspended || rtsp.paused) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
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
    if (rtsp.paused) return "PAUSED";
    if (status->reconnecting) return "RECONNECTING";
    if (!status->playing) return "CONNECTING";
    if (status->audio && !status->audio_playing) return "BUFFERING";
    return "PLAYING";
}

static void draw_controls(solar_os_gfx_t *gfx, int w, int h,
                          const solar_os_rtsp_client_status_t *status)
{
    char title[SOLAR_OS_RTSP_URL_MAX];
    if (solar_os_rtsp_url_redact(rtsp.url, title, sizeof(title)) != ESP_OK)
        snprintf(title, sizeof(title), "RTSP stream");
    if (rtsp.active < rtsp.playlist.count && rtsp.playlist.entries[rtsp.active].title[0])
        snprintf(title, sizeof(title), "%s", rtsp.playlist.entries[rtsp.active].title);
    solar_os_media_player_header_draw(gfx, w, "RTSP", "[Play]  Playlist");
    solar_os_media_player_controls_draw(gfx, w, h, title, playback_label(status),
        solar_os_media_preferences_volume(rtsp.preferences), false,
        rtsp.stopped ? SOLAR_OS_MEDIA_TRANSPORT_PLAY :
        rtsp.paused ? SOLAR_OS_MEDIA_TRANSPORT_PAUSE : SOLAR_OS_MEDIA_TRANSPORT_STOP);
}

static bool rtsp_playlist_entry(size_t index, solar_os_m3u_entry_t *entry, void *user)
{
    (void)user;
    if (index >= rtsp.playlist.count) return false;
    *entry = rtsp.playlist.entries[index];
    if (!entry->title[0]) {
        char redacted[SOLAR_OS_RTSP_URL_MAX];
        if (solar_os_rtsp_url_redact(entry->path, redacted, sizeof(redacted)) == ESP_OK)
            snprintf(entry->title, sizeof(entry->title), "%.95s", redacted);
        else snprintf(entry->title, sizeof(entry->title), "RTSP stream");
    }
    return true;
}

static void draw_playlist(solar_os_context_t *ctx)
{
    if (!rtsp.playlist_dirty) return;
    if (rtsp.graphical) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        int w = solar_os_gfx_width(gfx), h = solar_os_gfx_height(gfx);
        solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_media_player_header_draw(gfx, w, "RTSP", "Play  [Playlist]");
        solar_os_media_playlist_draw(gfx, w, h, rtsp.playlist.count, rtsp.cursor,
            rtsp.active, &rtsp.top, rtsp_playlist_entry, NULL,
            rtsp.message[0] ? rtsp.message : "A add  E edit  Del remove  Enter play");
        solar_os_gfx_present(gfx);
    } else {
        for (size_t i = 0; i < rtsp.playlist.count; i++) {
            solar_os_m3u_entry_t entry;
            rtsp_playlist_entry(i, &entry, NULL);
            solar_os_shell_io_printf(solar_os_context_shell_io(ctx), "%c %s\r\n",
                i == rtsp.cursor ? '>' : ' ', entry.title);
        }
        solar_os_shell_io_printf(solar_os_context_shell_io(ctx), "%s\r\n",
            rtsp.message[0] ? rtsp.message : "A add  E edit  Del remove  Enter play  Tab back");
    }
    rtsp.playlist_dirty = false;
}

static void draw_dialog(solar_os_context_t *ctx)
{
    if (!rtsp.playlist_dirty) return;
    const char *title = rtsp.dialog == 1 ? "Stream name" : "Stream URL";
    const char *help = rtsp.message[0] ? rtsp.message : "Enter next/save  Esc cancel";
    const char *input = rtsp.dialog_input;
    char redacted[SOLAR_OS_RTSP_URL_MAX];
    if (rtsp.dialog == 2 && strchr(input, '@')) {
        input = solar_os_rtsp_url_redact(input, redacted, sizeof(redacted)) == ESP_OK ?
            redacted : "URL with credentials";
    }
    if (rtsp.graphical) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        solar_os_media_dialog_draw(gfx, solar_os_gfx_width(gfx), solar_os_gfx_height(gfx),
            title, input, help);
        solar_os_gfx_present(gfx);
    } else {
        solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
        solar_os_shell_io_printf(io, "\r\033[K%s\r\n%s: %s\033[K", help, title, input);
        solar_os_shell_io_set_cursor_visible(io, true);
    }
    rtsp.playlist_dirty = false;
}

static void render(solar_os_context_t *ctx, bool force)
{
    if (rtsp.suspended) return;
    if (rtsp.dialog) { draw_dialog(ctx); return; }
    if (rtsp.playlist_visible) { draw_playlist(ctx); return; }
    if (!rtsp.graphical || rtsp.suspended) return;
    solar_os_rtsp_client_status_t status;
    playback_status(&status);
    bool changed = false;
    if (rtsp.paused && !force && !rtsp.dirty) return;
    uint64_t arrived = 0;
    xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
    while (rtsp.queued && !rtsp.paused) {
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
        .use_volume = true,
        .volume = solar_os_media_preferences_volume(rtsp.preferences),
        .device = solar_os_media_preferences_device,
        .user = rtsp.preferences,
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
    rtsp.paused = false;
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
    if (rtsp.client) solar_os_rtsp_client_cancel(rtsp.client);
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
    if (rtsp.stopped) {
        if (!rtsp.url[0] && rtsp.cursor < rtsp.playlist.count) {
            rtsp.active = rtsp.cursor;
            snprintf(rtsp.url, sizeof(rtsp.url), "%s", rtsp.playlist.entries[rtsp.active].path);
        }
        if (rtsp.url[0]) rtsp.restart = true;
    }
    else stop_playback(ctx);
    rtsp.dirty = true;
}

static void select_track(solar_os_context_t *ctx, size_t index)
{
    if (index >= rtsp.playlist.count) return;
    stop_playback(ctx);
    rtsp.active = rtsp.cursor = index;
    snprintf(rtsp.url, sizeof(rtsp.url), "%s", rtsp.playlist.entries[index].path);
    rtsp.restart = true;
    rtsp.playlist_dirty = true;
}

static void begin_dialog(bool edit)
{
    if (edit && rtsp.cursor >= rtsp.playlist.count) return;
    if (!edit && rtsp.playlist.count >= SOLAR_OS_MEDIA_PLAYLIST_CAPACITY) {
        snprintf(rtsp.message, sizeof(rtsp.message), "Playlist is full");
        return;
    }
    rtsp.edit_index = edit ? rtsp.cursor : SIZE_MAX;
    rtsp.dialog = 1;
    rtsp.dialog_input[0] = 0;
    if (edit) {
        solar_os_m3u_entry_t entry;
        rtsp_playlist_entry(rtsp.cursor, &entry, NULL);
        strlcpy(rtsp.dialog_input, entry.title, sizeof(rtsp.dialog_input));
    }
    rtsp.dialog_input_len = strlen(rtsp.dialog_input);
}

static void dialog_key(solar_os_context_t *ctx, uint8_t key)
{
    rtsp.message[0] = 0;
    solar_os_media_input_action_t action = solar_os_media_dialog_input_key(rtsp.dialog_input,
        rtsp.dialog == 1 ? sizeof(rtsp.dialog_name) : sizeof(rtsp.dialog_input), &rtsp.dialog_input_len, key);
    if (action == SOLAR_OS_MEDIA_INPUT_CANCEL) rtsp.dialog = 0;
    else if (action == SOLAR_OS_MEDIA_INPUT_SUBMIT) {
        if (!rtsp.dialog_input_len) snprintf(rtsp.message, sizeof(rtsp.message), "Value cannot be empty");
        else if (rtsp.dialog == 1) {
            strlcpy(rtsp.dialog_name, rtsp.dialog_input, sizeof(rtsp.dialog_name));
            rtsp.dialog = 2;
            strlcpy(rtsp.dialog_input, rtsp.edit_index < rtsp.playlist.count ?
                rtsp.playlist.entries[rtsp.edit_index].path : "", sizeof(rtsp.dialog_input));
            rtsp.dialog_input_len = strlen(rtsp.dialog_input);
        } else {
            solar_os_m3u_entry_t entry = {.duration = -1};
            esp_err_t err = solar_os_rtsp_url_normalize(rtsp.dialog_input, entry.path, sizeof(entry.path));
            size_t index = rtsp.edit_index == SIZE_MAX ? rtsp.playlist.count : rtsp.edit_index;
            strlcpy(entry.title, rtsp.dialog_name, sizeof(entry.title));
            if (err == ESP_OK) err = solar_os_media_playlist_set(&rtsp.playlist, index, &entry);
            if (err == ESP_OK) {
                if (rtsp.active == index) {
                    bool playing = !rtsp.stopped;
                    stop_playback(ctx);
                    strlcpy(rtsp.url, entry.path, sizeof(rtsp.url));
                    rtsp.restart = playing;
                }
                rtsp.cursor = index;
                rtsp.dialog = 0;
            } else snprintf(rtsp.message, sizeof(rtsp.message), "Cannot save stream: %s", esp_err_to_name(err));
        }
    }
    rtsp.playlist_dirty = rtsp.layout_dirty = true;
    render(ctx, true);
}

static void remove_selected(solar_os_context_t *ctx)
{
    size_t index = rtsp.cursor;
    if (index >= rtsp.playlist.count) return;
    esp_err_t err = solar_os_media_playlist_remove(&rtsp.playlist, index);
    if (err != ESP_OK) {
        snprintf(rtsp.message, sizeof(rtsp.message), "Cannot remove stream: %s", esp_err_to_name(err));
        return;
    }
    if (rtsp.active == index) {
        stop_playback(ctx);
        rtsp.active = SIZE_MAX;
        rtsp.url[0] = 0;
    } else if (rtsp.active != SIZE_MAX && rtsp.active > index) rtsp.active--;
    if (rtsp.cursor >= rtsp.playlist.count) rtsp.cursor = rtsp.playlist.count ? rtsp.playlist.count - 1U : 0;
}

static esp_err_t start(solar_os_context_t *ctx)
{
    rtsp.network_done = rtsp.decode_done = true;
    rtsp.stopped = true;
    rtsp.active = SIZE_MAX;
    const char *url = NULL;
    for (int i = 1; i < solar_os_context_argc(ctx); i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (!strcmp(arg, "--audio-only")) rtsp.audio_only = true;
        else if (!strcmp(arg, "--stats")) rtsp.diagnostics = true;
        else if (!url) url = arg;
        else return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_media_preferences_create(SOLAR_OS_MEDIA_RTSP, &rtsp.preferences);
    if (err != ESP_OK) return err;
    if (url && !solar_os_m3u_path(url))
        err = solar_os_rtsp_url_normalize(url, rtsp.url, sizeof(rtsp.url));
    if (err != ESP_OK) {
        solar_os_context_finish(ctx, 1,
            "RTSP: malformed/unsupported address; use [rtsp://][user[:pass]@]host[:port][/path] (no IPv6)");
        return err;
    }
    size_t selected = 0;
    err = solar_os_media_playlist_open(&rtsp.playlist, SOLAR_OS_MEDIA_PLAYLIST_RTSP,
        url && !solar_os_m3u_path(url) ? rtsp.url : url, &selected);
    if (err != ESP_OK) return err;
    rtsp.cursor = selected;
    rtsp.playlist_visible = !url || !rtsp.playlist.count;
    rtsp.playlist_dirty = true;
    if (!rtsp.playlist_visible) {
        rtsp.active = selected;
        snprintf(rtsp.url, sizeof(rtsp.url), "%s", rtsp.playlist.entries[rtsp.active].path);
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
    if (!rtsp.playlist_visible) err = start_playback(ctx);
    if (err != ESP_OK) return err;
    rtsp.ui_started = true;
    char redacted[SOLAR_OS_RTSP_URL_MAX];
    if (solar_os_rtsp_url_redact(rtsp.url, redacted, sizeof(redacted)) != ESP_OK)
        snprintf(redacted, sizeof(redacted), "%s", rtsp.url);
    if (!rtsp.graphical && !rtsp.playlist_visible)
        solar_os_shell_io_printf(io, "RTSP %s (audio only on port shell)\r\n", redacted);
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
    solar_os_media_preferences_destroy(rtsp.preferences);
    solar_os_media_playlist_close(&rtsp.playlist);
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
    rtsp.playlist_dirty = true;
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
    if (rtsp.fullscreen || rtsp.playlist_visible || rtsp.dialog || event->action != SOLAR_OS_INPUT_POINTER_PRESS ||
        !(event->buttons & SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY))
        return false;
    int button = solar_os_media_player_button_at(w, h, false, rtsp.pointer_x, rtsp.pointer_y);
    if (button < 0) return false;
    if (button == 1) toggle_playback(ctx);
    else if (button == 0 && rtsp.active) select_track(ctx, rtsp.active - 1);
    else if (button == 2 && rtsp.active + 1 < rtsp.playlist.count) select_track(ctx, rtsp.active + 1);
    return true;
}

static bool event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event->type == SOLAR_OS_EVENT_RESUME) { resume(ctx); return true; }
    if (event->type == SOLAR_OS_EVENT_POINTER) return pointer(ctx, &event->data.pointer);
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        uint8_t key = event->data.ch;
        if (rtsp.dialog && key != SOLAR_OS_KEY_APP_EXIT) { dialog_key(ctx, key); return true; }
        if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE || key == 'q' || key == 'Q')
            solar_os_context_finish(ctx, 0, NULL);
        else if (key == '\t') {
            rtsp.playlist_visible = !rtsp.playlist_visible;
            rtsp.playlist_dirty = rtsp.layout_dirty = true;
            render(ctx, true);
        } else if (key == ' ' && rtsp.client && !rtsp.stopped) {
            rtsp.paused = !rtsp.paused;
            solar_os_rtsp_client_set_paused(rtsp.client, rtsp.paused);
            rtsp.dirty = true;
        } else if (rtsp.playlist_visible) {
            rtsp.message[0] = 0;
            if (key == SOLAR_OS_KEY_UP && rtsp.cursor) rtsp.cursor--;
            else if (key == SOLAR_OS_KEY_DOWN && rtsp.cursor + 1 < rtsp.playlist.count) rtsp.cursor++;
            else if ((key == '\r' || key == '\n') && rtsp.cursor < rtsp.playlist.count) {
                select_track(ctx, rtsp.cursor);
                rtsp.playlist_visible = false;
            } else if (key == 'a' || key == 'A') begin_dialog(false);
            else if (key == 'e' || key == 'E') begin_dialog(true);
            else if (key == SOLAR_OS_KEY_DELETE || key == 0x7fU) remove_selected(ctx);
            rtsp.playlist_dirty = true;
            render(ctx, true);
        } else if (key == '\r' || key == '\n' || key == SOLAR_OS_KEY_ENTER)
            toggle_playback(ctx);
        else if (key == SOLAR_OS_KEY_LEFT && rtsp.active != SIZE_MAX && rtsp.active) select_track(ctx, rtsp.active - 1);
        else if (key == SOLAR_OS_KEY_RIGHT && rtsp.active != SIZE_MAX && rtsp.active + 1 < rtsp.playlist.count)
            select_track(ctx, rtsp.active + 1);
        else if (key == SOLAR_OS_KEY_UP || key == SOLAR_OS_KEY_DOWN || key == '+' || key == '-') {
            int volume = solar_os_media_preferences_volume(rtsp.preferences) +
                (key == SOLAR_OS_KEY_UP || key == '+' ? 5 : -5);
            if (solar_os_media_preferences_set_volume(rtsp.preferences,
                    volume < 0 ? 0 : volume > 100 ? 100 : volume) != ESP_OK)
                snprintf(rtsp.message, sizeof(rtsp.message), "Volume control unavailable");
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
        if (status.error == ESP_OK && rtsp.active + 1 < rtsp.playlist.count) {
            select_track(ctx, rtsp.active + 1);
            return true;
        }
        if (status.error == ESP_OK) {
            stop_playback(ctx);
            render(ctx, true);
            return true;
        }
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
            playback_label(&status), (unsigned long)status.sample_rate,
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
