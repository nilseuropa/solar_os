#include "solar_os_vplay.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_audio_pcm.h"
#include "solar_os_audio_player.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_display.h"
#include "solar_os_gfx.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_media_widgets.h"
#include "solar_os_mpeg.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define VPLAY_STACK 12288U
typedef struct mpeg_player mpeg_player_t;
static void mpeg_player_destroy(mpeg_player_t *p);

#define AUDIO_LEAD 0.50
#define LATE_US 120000
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(VPLAY_STACK);

struct mpeg_player {
    TaskHandle_t task;
    SemaphoreHandle_t mutex;
    volatile bool stop, done, suspended, paused;
    volatile bool seeking;
    int64_t seek_yield_at;
    bool fit, fullscreen, stopped, color, pending, dirty, layout_dirty, audio_running, audio_eof, diagnostics,
        high_refresh, simd;
    char display_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    uint32_t screen_width, screen_height, viewport_x, viewport_y, viewport_width, viewport_height, layout;
    uint8_t *current, *next;
    uint32_t current_w, current_h, next_w, next_h;
    size_t current_capacity, next_capacity;
    int64_t next_time, wall_start, pause_start, sink_at, sink_time, sink_quantum;
    uint64_t sink_frames;
    uint32_t sample_rate;
    double start_time;
    solar_os_mpeg_t *decoder;
    solar_os_audio_player_t *audio;
    solar_os_mpeg_info_t info;
    solar_os_stream_audio_format_t output;
    solar_os_audio_s16_converter_t resampler;
    int16_t *pcm, *converted;
    esp_err_t error;
    char detail[96], path[SOLAR_OS_STORAGE_PATH_MAX];
    uint32_t decoded, displayed, dropped;
    uint64_t decode_us, convert_us, present_us;
    int64_t last_stats;
    int64_t displayed_second;
};

static bool cancelled(void *user)
{
    mpeg_player_t *p = user;
    if (p->seeking && esp_timer_get_time() - p->seek_yield_at >= 10000) {
        /* Seeking has no display/audio waits to let the idle task run. */
        vTaskDelay(1);
        p->seek_yield_at = esp_timer_get_time();
    }
    return p->stop;
}
static void refresh(mpeg_player_t *p, bool enabled)
{
    if (p->display_target[0] && p->high_refresh != enabled &&
        solar_os_display_set_high_refresh_override(p->display_target, enabled, 255U) == ESP_OK)
        p->high_refresh = enabled;
}
static bool paused(void *user)
{
    mpeg_player_t *p = user;
    return p->paused || p->suspended;
}
static int64_t clock_locked(mpeg_player_t *p)
{
    int64_t now = esp_timer_get_time();
    if (p->info.audio) {
        int64_t delta = (p->audio_running || (p->audio_eof && p->sink_frames)) && !paused(p)
                            ? now - p->sink_at
                            : 0;
        if (!p->audio_eof && delta > p->sink_quantum)
            delta = p->sink_quantum;
        return p->sink_time + delta;
    }
    if (!p->wall_start)
        return (int64_t)(p->start_time * 1000000);
    return (p->pause_start ? p->pause_start : now) - p->wall_start;
}
static void audio_state(bool playing, void *user)
{
    mpeg_player_t *p = user;
    xSemaphoreTake(p->mutex, portMAX_DELAY);
    if (!playing)
        p->sink_time = clock_locked(p);
    p->audio_running = playing;
    p->sink_at = esp_timer_get_time();
    xSemaphoreGive(p->mutex);
}
static void audio_samples(const int16_t *samples, size_t count, uint8_t channels, void *user)
{
    (void)samples;
    mpeg_player_t *p = user;
    xSemaphoreTake(p->mutex, portMAX_DELAY);
    p->sink_time = p->sink_frames * 1000000ULL / p->sample_rate;
    p->sink_quantum = (count / channels) * 1000000ULL / p->sample_rate;
    p->sink_frames += count / channels;
    p->sink_at = esp_timer_get_time();
    p->audio_running = !paused(p);
    xSemaphoreGive(p->mutex);
}
static void failure(mpeg_player_t *p, esp_err_t err, const char *text)
{
    xSemaphoreTake(p->mutex, portMAX_DELAY);
    if (p->error == ESP_OK && !p->stop) {
        p->error = err;
        snprintf(p->detail, sizeof(p->detail), "%s", text);
        SOLAR_OS_LOGE("vplay", "%s (%s)", text, esp_err_to_name(err));
    }
    xSemaphoreGive(p->mutex);
}
static esp_err_t write_audio(mpeg_player_t *p, const solar_os_mpeg_audio_t *a)
{
    for (size_t i = 0; i < a->frames * 2U; i++) {
        float v = a->samples[i] * 32767.0f;
        p->pcm[i] = !isfinite(v) ? 0 : v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v;
    }
    const solar_os_stream_audio_format_t input = {.sample_rate = a->sample_rate,
                                                  .channels = 2,
                                                  .bits_per_sample = 16,
                                                  .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE};
    bool done = false;
    while (!done && !p->stop) {
        size_t count = 0;
        esp_err_t err = solar_os_audio_s16_convert(&p->resampler, p->pcm, a->frames, &input,
                                                   &p->output, p->converted, 2048, &count, &done);
        if (err != ESP_OK)
            return err;
        if (count) {
            err = solar_os_audio_player_write(p->audio, p->converted, count * sizeof(int16_t),
                                              &p->stop);
            if (err != ESP_OK)
                return err;
        }
    }
    return ESP_OK;
}
static bool wait_running(mpeg_player_t *p)
{
    while (paused(p) && !p->stop)
        vTaskDelay(pdMS_TO_TICKS(10));
    return !p->stop;
}
static esp_err_t feed_audio(mpeg_player_t *p, bool *ended, double *until, double deadline)
{
    while (!*ended && *until < deadline && !p->stop) {
        solar_os_mpeg_audio_t a;
        esp_err_t err = solar_os_mpeg_audio(p->decoder, &a, ended);
        if (err != ESP_OK) {
            failure(p, err, solar_os_mpeg_error(p->decoder));
            return err;
        }
        if (!*ended) {
            err = write_audio(p, &a);
            if (err != ESP_OK) {
                failure(p, err, "audio output write/resampling failed");
                return err;
            }
            *until = a.time + (double)a.frames / a.sample_rate;
        } else {
            /* Signal the finite tail immediately, without waiting for video. */
            err = solar_os_audio_player_end_input(p->audio);
            if (err != ESP_OK) {
                failure(p, err, "audio output end-of-input failed");
                return err;
            }
            xSemaphoreTake(p->mutex, portMAX_DELAY);
            p->audio_eof = true;
            xSemaphoreGive(p->mutex);
        }
    }
    return ESP_OK;
}
static void worker(void *user)
{
    mpeg_player_t *p = user;
    char detail[96] = {0};
    p->simd = p->color && solar_os_mpeg_simd_selftest();
    if (p->color && solar_os_mpeg_simd_available() && !p->simd)
        SOLAR_OS_LOGW("vplay", "SIMD conversion self-test failed; using CPU fallback");
    esp_err_t err = solar_os_mpeg_open(p->path, cancelled, p, &p->decoder, detail, sizeof(detail));
    if (err != ESP_OK) {
        failure(p, err, detail);
        goto end;
    }
    solar_os_mpeg_info_t info;
    solar_os_mpeg_info(p->decoder, &info);
    double position = p->start_time;
    if (p->start_time > 0) {
        p->seeking = true;
        err = solar_os_mpeg_seek(p->decoder, p->start_time, &position);
        p->seeking = false;
        if (err != ESP_OK) {
            failure(p, err, solar_os_mpeg_error(p->decoder));
            goto end;
        }
    }
    xSemaphoreTake(p->mutex, portMAX_DELAY);
    p->start_time = position;
    p->info = info;
    p->layout_dirty = true;
    xSemaphoreGive(p->mutex);
    if (info.audio) {
        p->pcm = solar_os_memory_alloc(1152 * 2 * sizeof(int16_t),
                                       SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "vplay.mp2");
        p->converted = solar_os_memory_alloc(2048 * sizeof(int16_t),
                                             SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "vplay.pcm");
        if (!p->pcm || !p->converted) {
            failure(p, ESP_ERR_NO_MEM, "MP2 PCM PSRAM allocation failed");
            goto end;
        }
        solar_os_audio_player_options_t options = {.owner = "vplay",
                                                   .volume = SOLAR_OS_AUDIO_VOLUME_GLOBAL,
                                                   .buffered = true,
                                                   .external_buffer_bytes = 32768,
                                                   .internal_buffer_bytes = 4096,
                                                   .target_ms = 250,
                                                   .open_timeout_ms = 1000,
                                                   .state = audio_state,
                                                   .samples = audio_samples,
                                                   .user = p,
                                                   .should_cancel = cancelled,
                                                   .cancel_user = p,
                                                   .should_pause = paused,
                                                   .pause_user = p};
        err = solar_os_audio_player_create(&options, &p->audio, &p->output, NULL);
        if (err != ESP_OK) {
            failure(p, err, "audio output unavailable, busy, or insufficient task SRAM");
            goto end;
        }
        xSemaphoreTake(p->mutex, portMAX_DELAY);
        p->sample_rate = p->output.sample_rate;
        p->sink_frames = (uint64_t)(p->start_time * p->sample_rate);
        p->sink_time = (int64_t)(p->start_time * 1000000);
        xSemaphoreGive(p->mutex);
    }
    if (p->start_time > 0 && !info.audio) {
        xSemaphoreTake(p->mutex, portMAX_DELAY);
        p->wall_start = esp_timer_get_time() - (int64_t)(p->start_time * 1000000);
        if (paused(p)) p->pause_start = esp_timer_get_time();
        xSemaphoreGive(p->mutex);
    }
    bool video_end = false, audio_end = !info.audio;
    double audio_until = 0, video_until = 0;
    while (!p->stop && !video_end) {
        if (!wait_running(p))
            break;
        int64_t started = esp_timer_get_time();
        solar_os_mpeg_frame_t frame;
        err = solar_os_mpeg_video(p->decoder, &frame, &video_end);
        if (err != ESP_OK) {
            failure(p, err, solar_os_mpeg_error(p->decoder));
            break;
        }
        if (video_end)
            break;
        p->decode_us += esp_timer_get_time() - started;
        p->decoded++;
        video_until = frame.time + 1.0 / info.fps;
        xSemaphoreTake(p->mutex, portMAX_DELAY);
        double audio_deadline = clock_locked(p) / 1000000.0;
        xSemaphoreGive(p->mutex);
        if (audio_deadline < frame.time)
            audio_deadline = frame.time;
        if (feed_audio(p, &audio_end, &audio_until, audio_deadline + AUDIO_LEAD) != ESP_OK)
            goto end;
        bool free_slot = false;
        while (!p->stop && !free_slot) {
            if (!wait_running(p))
                break;
            xSemaphoreTake(p->mutex, portMAX_DELAY);
            free_slot = !p->pending;
            double pending_deadline = clock_locked(p) / 1000000.0 + AUDIO_LEAD;
            xSemaphoreGive(p->mutex);
            if (!free_slot) {
                /* A slow display must not stop replenishing the audio clock.
                 * Audio decoding leaves this track's video planes intact. */
                if (feed_audio(p, &audio_end, &audio_until, pending_deadline) != ESP_OK)
                    goto end;
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
        if (p->stop)
            break;
        xSemaphoreTake(p->mutex, portMAX_DELAY);
        int64_t clock = clock_locked(p);
        uint32_t generation = p->layout, w = info.width, h = info.height;
        bool fit = p->fit;
        if (fit) {
            w = p->viewport_width;
            h = (uint64_t)info.height * w / info.width;
            if (h > p->viewport_height) {
                h = p->viewport_height;
                w = (uint64_t)info.width * h / info.height;
            }
        } else {
            /* Actual size is cropped to the screen, without a driver scale. */
            if (w > p->viewport_width)
                w = p->viewport_width;
            if (h > p->viewport_height)
                h = p->viewport_height;
        }
        xSemaphoreGive(p->mutex);
        if ((int64_t)(frame.time * 1000000) + LATE_US < clock) {
            p->dropped++;
            continue;
        }
        if (!w)
            w = 1;
        if (!h)
            h = 1;
        size_t bytes = (size_t)w * h * (p->color ? 2U : 1U);
        if (bytes > p->next_capacity) {
            uint8_t *buffer = solar_os_memory_realloc(
                p->next, bytes, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "vplay.video");
            if (!buffer) {
                failure(p, ESP_ERR_NO_MEM, "video raster PSRAM allocation failed");
                break;
            }
            p->next = buffer;
            p->next_capacity = bytes;
        }
        started = esp_timer_get_time();
        /* Scale while converting; the display receives an unscaled raster. */
        solar_os_mpeg_frame_t raster_frame = frame;
        if (!fit) {
            unsigned left = ((frame.width - w) / 2) & ~1U, top = ((frame.height - h) / 2) & ~1U;
            raster_frame.y += top * frame.y_stride + left;
            raster_frame.cb += (top / 2) * frame.chroma_stride + left / 2;
            raster_frame.cr += (top / 2) * frame.chroma_stride + left / 2;
            raster_frame.width = w;
            raster_frame.height = h;
        }
        solar_os_mpeg_raster(&raster_frame, p->next, w, h, p->color, p->simd);
        p->convert_us += esp_timer_get_time() - started;
        xSemaphoreTake(p->mutex, portMAX_DELAY);
        if (generation == p->layout) {
            p->next_w = w;
            p->next_h = h;
            p->next_time = frame.time * 1000000;
            p->pending = true;
        }
        xSemaphoreGive(p->mutex);
        vTaskDelay(1);
    }
    if (!p->decoded && p->error == ESP_OK && !p->stop)
        failure(p, ESP_ERR_INVALID_ARG, "MPEG file contains no complete video pictures");
    /* Drain the remaining finite audio track, then wait for the last staged
     * video frame to reach its deadline before returning to the shell. */
    while (!p->stop && !audio_end && p->error == ESP_OK) {
        solar_os_mpeg_audio_t a;
        err = solar_os_mpeg_audio(p->decoder, &a, &audio_end);
        if (err != ESP_OK) {
            failure(p, err, solar_os_mpeg_error(p->decoder));
            break;
        }
        if (!audio_end && (err = write_audio(p, &a)) != ESP_OK) {
            failure(p, err, "audio output write failed");
            break;
        }
    }
    if (p->audio && p->error == ESP_OK && !p->stop) {
        err = solar_os_audio_player_finish(p->audio, &p->stop);
        if (err != ESP_OK)
            failure(p, err, "audio output drain failed");
    }
    while (!p->stop && p->error == ESP_OK) {
        xSemaphoreTake(p->mutex, portMAX_DELAY);
        bool pending = p->pending;
        int64_t clock = clock_locked(p);
        xSemaphoreGive(p->mutex);
        if (!pending && clock >= (int64_t)(video_until * 1000000))
            break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
end:
    solar_os_audio_player_destroy(p->audio);
    p->audio = NULL;
    solar_os_mpeg_close(p->decoder);
    p->decoder = NULL;
    solar_os_memory_free(p->pcm);
    p->pcm = NULL;
    solar_os_memory_free(p->converted);
    p->converted = NULL;
    p->done = true;
    for (;;)
        vTaskSuspend(NULL);
}
static void viewport(mpeg_player_t *p)
{
    solar_os_media_player_layout_t layout;
    solar_os_media_player_layout(p->screen_width, p->screen_height, true, &layout);
    p->viewport_x = p->fullscreen ? 0U : (uint32_t)layout.view_x;
    p->viewport_y = p->fullscreen ? 0U : (uint32_t)layout.view_y;
    p->viewport_width = p->fullscreen ? p->screen_width : (uint32_t)layout.view_width;
    p->viewport_height = p->fullscreen ? p->screen_height : (uint32_t)layout.view_height;
}
static esp_err_t mpeg_player_start(solar_os_context_t *ctx, const char *path, bool fit,
                                   bool fullscreen, double start_time, bool paused,
                                   mpeg_player_t **out)
{
    *out = NULL;
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (!gfx || solar_os_gfx_width(gfx) < 84U || solar_os_gfx_height(gfx) < 48U)
        return ESP_ERR_NOT_SUPPORTED;
    solar_os_media_player_layout_t layout;
    solar_os_media_player_layout(solar_os_gfx_width(gfx), solar_os_gfx_height(gfx), true, &layout);
    if (!fullscreen && (layout.view_width < 2 || layout.view_height < 2))
        return ESP_ERR_NOT_SUPPORTED;
    mpeg_player_t *p =
        solar_os_memory_calloc(1, sizeof(*p), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "vplay.state");
    if (!p)
        return ESP_ERR_NO_MEM;
    p->mutex = xSemaphoreCreateMutex();
    if (!p->mutex) {
        solar_os_memory_free(p);
        return ESP_ERR_NO_MEM;
    }
    p->fit = fit;
    p->fullscreen = fullscreen;
    p->start_time = start_time;
    p->seeking = start_time > 0;
    p->paused = paused;
    p->color = solar_os_gfx_format(gfx) == SOLAR_OS_DISPLAY_FORMAT_INDEX8;
    p->screen_width = solar_os_gfx_width(gfx);
    p->screen_height = solar_os_gfx_height(gfx);
    viewport(p);
    p->layout_dirty = true;
    p->last_stats = esp_timer_get_time();
    snprintf(p->path, sizeof(p->path), "%s", path);
    (void)solar_os_gfx_display_target_name(gfx, p->display_target, sizeof(p->display_target));
    *out = p;
    if (solar_os_task_create_pinned_internal(worker, "vplay-decode", VPLAY_STACK, p,
                                             tskIDLE_PRIORITY + 1, &p->task, tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        p->done = true;
        mpeg_player_destroy(p);
        *out = NULL;
        return ESP_ERR_NO_MEM;
    }
    refresh(p, true);
    return ESP_OK;
}
static void mpeg_player_stop(mpeg_player_t *p)
{
    if (!p)
        return;
    refresh(p, false);
    p->stop = true;
    (void)solar_os_task_wait_done(p->task, &p->done, SOLAR_OS_TASK_STOP_WAIT_MS);
}
static bool mpeg_player_ready(const mpeg_player_t *p) { return !p || p->done; }
static void mpeg_player_destroy(mpeg_player_t *p)
{
    if (!p || !p->done)
        return;
    if (p->task)
        solar_os_task_delete_internal(p->task);
    if (p->mutex)
        vSemaphoreDelete(p->mutex);
    solar_os_memory_free(p->current);
    solar_os_memory_free(p->next);
    solar_os_memory_free(p);
}
static void mpeg_player_pause(mpeg_player_t *p, bool suspended)
{
    refresh(p, !suspended && !p->stopped);
    xSemaphoreTake(p->mutex, portMAX_DELAY);
    bool before = paused(p);
    p->suspended = suspended;
    bool after = paused(p);
    if (!before && after)
        p->pause_start = esp_timer_get_time();
    if (before && !after && p->pause_start) {
        if (p->wall_start)
            p->wall_start += esp_timer_get_time() - p->pause_start;
        p->pause_start = 0;
    }
    p->layout_dirty = true;
    xSemaphoreGive(p->mutex);
}
static void mpeg_player_fit(mpeg_player_t *p, bool fit)
{
    xSemaphoreTake(p->mutex, portMAX_DELAY);
    p->fit = fit;
    p->layout++;
    p->pending = false;
    p->layout_dirty = true;
    xSemaphoreGive(p->mutex);
}
static void mpeg_player_fullscreen(mpeg_player_t *p, bool fullscreen)
{
    xSemaphoreTake(p->mutex, portMAX_DELAY);
    p->fullscreen = fullscreen;
    viewport(p);
    p->layout++;
    p->pending = false;
    p->layout_dirty = true;
    xSemaphoreGive(p->mutex);
}
static const char *vplay_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}
static void playback_status_text(mpeg_player_t *p, int64_t second, char *status, size_t capacity)
{
    const char *state = p->stopped ? "STOPPED" : p->seeking ? "SEEKING" :
                        paused(p) ? "PAUSED" : "PLAYING";
    snprintf(status, capacity, "%s %02llu:%02llu", state,
             (unsigned long long)(second / 60), (unsigned long long)(second % 60));
}
static bool draw_play_time(mpeg_player_t *p, solar_os_gfx_t *gfx, int64_t clock, bool force)
{
    if (p->fullscreen)
        return false;
    int64_t second = p->stopped ? p->displayed_second : clock > 0 ? clock / 1000000 : 0;
    if (!force && second == p->displayed_second)
        return false;
    p->displayed_second = second;
    /* Redraw only the status row: never clear or re-rasterize the video. */
    char status[48];
    playback_status_text(p, second, status, sizeof(status));
    solar_os_media_player_status_draw(gfx, p->screen_width, p->screen_height, status);
    return true;
}
static void draw_chrome(mpeg_player_t *p, solar_os_gfx_t *gfx, int64_t clock)
{
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    if (!p->fullscreen) {
        const int w = p->screen_width, h = p->screen_height;
        char metadata[72];
        snprintf(metadata, sizeof(metadata), "%lux%lu %.1ffps %s",
                 (unsigned long)p->info.width, (unsigned long)p->info.height,
                 p->info.fps, p->fit ? "FIT" : "ACTUAL");
        solar_os_media_player_header_draw(gfx, w, "VPlay", metadata);
        p->displayed_second = p->stopped ? p->displayed_second : clock > 0 ? clock / 1000000 : 0;
        char status[48];
        playback_status_text(p, p->displayed_second, status, sizeof(status));
        solar_os_audio_status_t audio;
        solar_os_audio_get_status(&audio);
        solar_os_media_player_controls_draw(gfx, w, h, vplay_basename(p->path), status,
            audio.volume, true, p->stopped ? SOLAR_OS_MEDIA_TRANSPORT_PLAY :
            paused(p) ? SOLAR_OS_MEDIA_TRANSPORT_PAUSE : SOLAR_OS_MEDIA_TRANSPORT_STOP);
    } else if (p->seeking) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
        solar_os_gfx_text(gfx,
            ((int)p->screen_width - (int)solar_os_gfx_text_width(gfx, "SEEKING")) / 2,
            p->screen_height / 2, "SEEKING");
    }
    solar_os_gfx_present(gfx);
}
static void mpeg_player_event(solar_os_context_t *ctx, mpeg_player_t *p,
                              const solar_os_event_t *event)
{
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        uint8_t key = event->data.ch;
        if (key == SOLAR_OS_KEY_UP || key == SOLAR_OS_KEY_DOWN) {
            solar_os_audio_status_t status;
            solar_os_audio_get_status(&status);
            int v = status.volume + (key == SOLAR_OS_KEY_UP ? 5 : -5);
            solar_os_audio_set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
            p->layout_dirty = true;
        } else if (key == ' ') {
            if (p->stopped || p->done) return;
            xSemaphoreTake(p->mutex, portMAX_DELAY);
            if (!paused(p))
                p->pause_start = esp_timer_get_time();
            p->paused = !p->paused;
            if (!paused(p) && p->pause_start) {
                if (p->wall_start)
                    p->wall_start += esp_timer_get_time() - p->pause_start;
                p->pause_start = 0;
            }
            p->layout_dirty = true;
            xSemaphoreGive(p->mutex);
        } else if (key == 'd' || key == 'D') {
            p->diagnostics = !p->diagnostics;
        }
        return;
    }
    if (event->type != SOLAR_OS_EVENT_TICK || p->suspended)
        return;
    xSemaphoreTake(p->mutex, portMAX_DELAY);
    if (p->error != ESP_OK) {
        char message[160];
        snprintf(message, sizeof(message), "vplay: %s (%s)", p->detail,
                 esp_err_to_name(p->error));
        xSemaphoreGive(p->mutex);
        solar_os_context_finish(ctx, 1, message);
        return;
    }
    if (p->done && !p->stopped) {
        xSemaphoreGive(p->mutex);
        solar_os_context_finish(ctx, 0, NULL);
        return;
    }
    if (!p->wall_start && p->pending && !p->info.audio)
        p->wall_start = esp_timer_get_time();
    int64_t clock = clock_locked(p);
    if (p->pending && !p->stopped && !paused(p) && p->next_time <= clock + 5000) {
        uint8_t *old = p->current;
        size_t capacity = p->current_capacity;
        p->current = p->next;
        p->current_capacity = p->next_capacity;
        p->current_w = p->next_w;
        p->current_h = p->next_h;
        p->next = old;
        p->next_capacity = capacity;
        p->pending = false;
        p->dirty = true;
    }
    bool dirty = p->dirty, chrome = p->layout_dirty;
    int64_t display_time = p->seeking ? (int64_t)(p->start_time * 1000000) : clock;
    p->dirty = false;
    p->layout_dirty = false;
    xSemaphoreGive(p->mutex);
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (chrome)
        draw_chrome(p, gfx, display_time);
    else if (draw_play_time(p, gfx, display_time, false))
        solar_os_gfx_present(gfx);
    if ((dirty || chrome) && p->current && p->current_w <= p->viewport_width &&
        p->current_h <= p->viewport_height) {
        int64_t started = esp_timer_get_time();
        int x = p->viewport_x + (p->viewport_width - p->current_w) / 2;
        int y = p->viewport_y + (p->viewport_height - p->current_h) / 2;
        esp_err_t err;
        if (p->color) {
            solar_os_display_raster_t raster = {.data = p->current,
                                                .data_size =
                                                    (size_t)p->current_w * p->current_h * 2,
                                                .source_width = p->current_w,
                                                .source_height = p->current_h,
                                                .source_stride = p->current_w * 2,
                                                .x = x,
                                                .y = y,
                                                .width = p->current_w,
                                                .height = p->current_h,
                                                .format = SOLAR_OS_DISPLAY_FORMAT_RGB565};
            err = solar_os_gfx_present_frame(gfx, &raster);
        } else {
            solar_os_gfx_raster_t raster = {.pixels = p->current,
                                            .pixels_size = (size_t)p->current_w * p->current_h,
                                            .width = p->current_w,
                                            .height = p->current_h,
                                            .stride = p->current_w,
                                            .format = SOLAR_OS_GFX_RASTER_GRAY8};
            err = solar_os_gfx_blit_raster(gfx, &raster, x, y, p->current_w, p->current_h, NULL);
            if (err == ESP_OK)
                solar_os_gfx_present(gfx);
        }
        if (err != ESP_OK)
            failure(p, err, "video display transfer/rotation failed");
        p->present_us += esp_timer_get_time() - started;
        if (dirty)
            p->displayed++;
    }
    if (p->diagnostics && esp_timer_get_time() - p->last_stats >= 1000000) {
        SOLAR_OS_LOGI("vplay",
                      "decoded=%lu shown=%lu dropped=%lu convert=%s decode_us=%llu convert_us=%llu "
                      "present_us=%llu stack_free=%u",
                      (unsigned long)p->decoded, (unsigned long)p->displayed,
                      (unsigned long)p->dropped, p->simd ? "simd" : "cpu", p->decode_us,
                      p->convert_us, p->present_us,
                      p->task ? (unsigned)uxTaskGetStackHighWaterMark(p->task) : 0U);
        p->last_stats = esp_timer_get_time();
    }
}

typedef struct {
    mpeg_player_t *player;
    bool fit, fullscreen, restart;
    bool restart_paused;
    double restart_time;
    char next_path[SOLAR_OS_STORAGE_PATH_MAX];
    int16_t pointer_x, pointer_y;
} vplay_state_t;
static void *vplay_state_storage;
#define vplay_state (*(vplay_state_t *)vplay_state_storage)

static esp_err_t vplay_start(solar_os_context_t *ctx)
{
    memset(&vplay_state, 0, sizeof(vplay_state));
    vplay_state.fit = true;
    const char *path_arg = NULL;
    for (int i = 1; i < solar_os_context_argc(ctx); i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (!arg)
            goto usage;
        if (!strcmp(arg, "-fit") || !strcmp(arg, "--fit"))
            vplay_state.fit = true;
        else if (!strcmp(arg, "-actual") || !strcmp(arg, "--actual"))
            vplay_state.fit = false;
        else if (!path_arg && arg[0] != '-')
            path_arg = arg;
        else
            goto usage;
    }
    if (!path_arg)
        goto usage;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_resolve_path(path_arg, path, sizeof(path));
    if (err != ESP_OK) {
        solar_os_context_finish(ctx, 1, "vplay: invalid file path");
        return ESP_OK;
    }
    solar_os_context_set_graphics_active(ctx, true);
    err = mpeg_player_start(ctx, path, vplay_state.fit, vplay_state.fullscreen, 0, false,
                            &vplay_state.player);
    if (err != ESP_OK) {
        solar_os_context_set_graphics_active(ctx, false);
        solar_os_context_finish(
            ctx, 1,
            err == ESP_ERR_NO_MEM
                ? "vplay: insufficient PSRAM or internal SRAM for the 12 KiB decoder task"
                : "vplay: a graphics display is required");
    }
    return ESP_OK;
usage:
    solar_os_context_finish(ctx, 2, "usage: vplay [-fit|-actual] <file.mpg>");
    return ESP_OK;
}
static void vplay_stop(solar_os_context_t *ctx)
{
    mpeg_player_stop(vplay_state.player);
    solar_os_context_set_graphics_active(ctx, false);
}
static bool vplay_release_ready(void) { return mpeg_player_ready(vplay_state.player); }
static void vplay_release_cleanup(void)
{
    mpeg_player_destroy(vplay_state.player);
    vplay_state.player = NULL;
}
static void vplay_suspend(solar_os_context_t *ctx)
{
    if (vplay_state.player)
        mpeg_player_pause(vplay_state.player, true);
    solar_os_context_set_graphics_active(ctx, false);
}
static void vplay_resume(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, true);
    if (vplay_state.player)
        mpeg_player_pause(vplay_state.player, false);
}
static void vplay_reap_stopped(mpeg_player_t *p)
{
    if (p->stopped && p->done && p->task) {
        solar_os_task_delete_internal(p->task);
        p->task = NULL;
    }
}
static void vplay_stop_playback(mpeg_player_t *p)
{
    xSemaphoreTake(p->mutex, portMAX_DELAY);
    int64_t clock = clock_locked(p);
    p->displayed_second = clock > 0 ? clock / 1000000 : 0;
    p->stopped = true;
    p->layout_dirty = true;
    xSemaphoreGive(p->mutex);
    mpeg_player_stop(p);
    vplay_reap_stopped(p);
}
static void vplay_restart(const char *path)
{
    snprintf(vplay_state.next_path, sizeof(vplay_state.next_path), "%s", path);
    vplay_state.restart = true;
    vplay_state.restart_time = 0;
    vplay_state.restart_paused = false;
    vplay_stop_playback(vplay_state.player);
}
static void vplay_seek(int direction)
{
    mpeg_player_t *p = vplay_state.player;
    if (p->stopped || p->done) return;
    xSemaphoreTake(p->mutex, portMAX_DELAY);
    double target = clock_locked(p) / 1000000.0 + direction * 10.0;
    bool paused = p->paused;
    xSemaphoreGive(p->mutex);
    vplay_restart(p->path);
    vplay_state.restart_time = target < 0 ? 0 : target;
    vplay_state.restart_paused = paused;
}
static int compare_names(const char *a, const char *b)
{
    int result = strcasecmp(a, b);
    return result ? result : strcmp(a, b);
}
static void vplay_neighbor(int direction)
{
    mpeg_player_t *p = vplay_state.player;
    char directory[SOLAR_OS_STORAGE_PATH_MAX], selected[SOLAR_OS_STORAGE_PATH_MAX] = {0};
    snprintf(directory, sizeof(directory), "%s", p->path);
    char *slash = strrchr(directory, '/');
    if (!slash)
        return;
    if (slash == directory)
        slash[1] = 0;
    else
        *slash = 0;
    const char *current = vplay_basename(p->path);
    /* Page through the storage service; no retained or unbounded playlist. */
    size_t cursor = 0;
    bool more = true;
    while (more) {
        solar_os_storage_entry_t entries[4];
        size_t count = 0, next = 0;
        esp_err_t err = solar_os_storage_scandir(directory, cursor, 4, entries, &count, &next, &more);
        if (err != ESP_OK || (more && next <= cursor)) {
            SOLAR_OS_LOGW("vplay", "cannot select neighboring video: %s", esp_err_to_name(err));
            return;
        }
        cursor = next;
        for (size_t i = 0; i < count; i++) {
            const char *name = entries[i].name, *extension = strrchr(name, '.');
            if (entries[i].metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE || !extension ||
                (strcasecmp(extension, ".mpg") && strcasecmp(extension, ".mpeg")))
                continue;
            int relative = compare_names(name, current);
            if ((direction < 0 ? relative < 0 : relative > 0) &&
                (!selected[0] || (direction < 0 ? compare_names(name, selected) > 0
                                               : compare_names(name, selected) < 0)))
                snprintf(selected, sizeof(selected), "%s", name);
        }
    }
    if (!selected[0])
        return;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_join_path(directory, selected, path, sizeof(path)) == ESP_OK)
        vplay_restart(path);
}
static bool vplay_transport(uint8_t key)
{
    mpeg_player_t *p = vplay_state.player;
    if (vplay_state.restart)
        return true;
    if (key == '<' || key == '>')
        vplay_seek(key == '<' ? -1 : 1);
    else if (key == SOLAR_OS_KEY_LEFT)
        vplay_neighbor(-1);
    else if (key == SOLAR_OS_KEY_RIGHT)
        vplay_neighbor(1);
    else if (key == '\r' || key == '\n' || key == SOLAR_OS_KEY_ENTER) {
        if (p->stopped)
            vplay_restart(p->path);
        else
            vplay_stop_playback(p);
    } else
        return false;
    return true;
}
static bool vplay_pointer(const solar_os_input_pointer_event_t *event)
{
    mpeg_player_t *p = vplay_state.player;
    if (event->mode == SOLAR_OS_INPUT_POINTER_ABSOLUTE) {
        vplay_state.pointer_x = event->x;
        vplay_state.pointer_y = event->y;
    } else {
        int x = vplay_state.pointer_x + event->delta_x, y = vplay_state.pointer_y + event->delta_y;
        vplay_state.pointer_x = x < 0 ? 0 : x >= (int)p->screen_width ? p->screen_width - 1 : x;
        vplay_state.pointer_y = y < 0 ? 0 : y >= (int)p->screen_height ? p->screen_height - 1 : y;
    }
    if (p->fullscreen || event->action != SOLAR_OS_INPUT_POINTER_PRESS ||
        !(event->buttons & SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY))
        return false;
    int button = solar_os_media_player_button_at(p->screen_width, p->screen_height, true,
                                                  vplay_state.pointer_x, vplay_state.pointer_y);
    static const uint8_t keys[] = {SOLAR_OS_KEY_LEFT, '<', '\r', '>', SOLAR_OS_KEY_RIGHT};
    return button >= 0 ? vplay_transport(keys[button]) : false;
}
static bool vplay_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (!event || !vplay_state.player)
        return false;
    if (event->type == SOLAR_OS_EVENT_CHAR && ((uint8_t)event->data.ch == SOLAR_OS_KEY_APP_EXIT ||
                                               (uint8_t)event->data.ch == SOLAR_OS_KEY_ESCAPE)) {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        uint8_t ch = event->data.ch;
        if (vplay_transport(ch))
            return true;
        if (ch == 'f' || ch == 'F') {
            vplay_state.fullscreen = !vplay_state.fullscreen;
            mpeg_player_fullscreen(vplay_state.player, vplay_state.fullscreen);
            return true;
        }
        if (ch == '0' || ch == '1') {
            vplay_state.fit = ch == '1';
            mpeg_player_fit(vplay_state.player, vplay_state.fit);
            return true;
        }
    }
    if (event->type == SOLAR_OS_EVENT_POINTER)
        return vplay_pointer(&event->data.pointer);
    if (event->type == SOLAR_OS_EVENT_TICK) {
        vplay_reap_stopped(vplay_state.player);
        if (vplay_state.restart && vplay_state.player->done) {
            mpeg_player_destroy(vplay_state.player);
            vplay_state.player = NULL;
            vplay_state.restart = false;
            esp_err_t err = mpeg_player_start(ctx, vplay_state.next_path, vplay_state.fit,
                                             vplay_state.fullscreen, vplay_state.restart_time,
                                             vplay_state.restart_paused, &vplay_state.player);
            if (err != ESP_OK) {
                char detail[96];
                snprintf(detail, sizeof(detail), "vplay: cannot restart decoder (%s)", esp_err_to_name(err));
                solar_os_context_finish(ctx, 1, detail);
            }
            return true;
        }
    }
    mpeg_player_event(ctx, vplay_state.player, event);
    return true;
}
static void vplay_title(solar_os_context_t *ctx, char *buffer, size_t size)
{
    (void)ctx;
    mpeg_player_t *p = vplay_state.player;
    if (!buffer || !size)
        return;
    const char *name = p ? strrchr(p->path, '/') : NULL;
    snprintf(buffer, size, "vplay %s", name ? name + 1 : p ? p->path : "");
}
const solar_os_app_t solar_os_vplay_app = {
    .name = "vplay",
    .summary = "MPEG-1 media player",
    .app_class = SOLAR_OS_APP_CLASS_GUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE | SOLAR_OS_APP_FLAG_POINTER_EVENTS,
    .start = vplay_start,
    .stop = vplay_stop,
    .suspend = vplay_suspend,
    .resume = vplay_resume,
    .event = vplay_event,
    .title = vplay_title,
    .state_slot = &vplay_state_storage,
    .state_size = sizeof(vplay_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .state_release_ready = vplay_release_ready,
    .state_release_cleanup = vplay_release_cleanup,
    .tick_interval_ms = 20,
};
