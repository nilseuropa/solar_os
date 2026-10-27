#include "solar_os_emotion.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "solar_os_config.h"
#include "solar_os_display.h"
#include "solar_os_frame_presenter.h"
#include "solar_os_gfx_internal.h"
#include "solar_os_memory.h"
#include "solar_os_native.h"
#include "solar_os_queue.h"
#include "solar_os_task.h"
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#include "solar_os_audio.h"
#endif

#define EMOTION_LIMIT 4U
#define EMOTION_QUEUE 8U
typedef struct {
    enum { COMMAND_SET, COMMAND_REACT, COMMAND_LOOK, COMMAND_BLINK } type;
    uint32_t pose, hold, transition, cue;
    int32_t x, y;
} emotion_command_t;
struct solar_os_emotion {
    const solar_os_emotion_api_v1_t *api;
    void *state;
    uint32_t lease;
    solar_os_gfx_t *gfx;
    solar_os_frame_presenter_t *presenter;
    char target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX], owner[24];
    char display_owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
    bool claimed, registered, sound, motion, foreground, software;
    atomic_bool paused;
    uint16_t width, height, stride, fps, palette[4];
    solar_os_gfx_color_t canvas_palette[4];
    uint8_t *pixels, *previous;
    bool have_frame;
    size_t bytes;
    QueueHandle_t commands;
    TaskHandle_t worker;
    volatile bool done;
    atomic_bool stop, redraw;
    atomic_uint frames, dropped, audio_skipped;
    atomic_int error, audio_error;
    uint64_t clock_offset, pause_started;
};
static portMUX_TYPE instances_lock = portMUX_INITIALIZER_UNLOCKED;
static solar_os_emotion_t *instances[EMOTION_LIMIT];
static uint32_t next_owner;

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000U; }
static esp_err_t result(int value)
{
    switch (value) {
    case SOLAR_OS_NATIVE_OK: return ESP_OK;
    case SOLAR_OS_NATIVE_ERROR_NO_MEMORY: return ESP_ERR_NO_MEM;
    case SOLAR_OS_NATIVE_ERROR_NOT_FOUND: return ESP_ERR_NOT_FOUND;
    case SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED: return ESP_ERR_NOT_SUPPORTED;
    case SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT: return ESP_ERR_INVALID_ARG;
    case SOLAR_OS_NATIVE_ERROR_TIMEOUT: return ESP_ERR_TIMEOUT;
    default: return ESP_ERR_INVALID_STATE;
    }
}
static esp_err_t register_instance(solar_os_emotion_t *s)
{
    esp_err_t err = ESP_ERR_NO_MEM;
    unsigned empty = EMOTION_LIMIT;
    uint32_t id = 0;
    portENTER_CRITICAL(&instances_lock);
    for (unsigned i = 0; i < EMOTION_LIMIT; ++i) {
        if (!instances[i]) empty = i;
        else if (!strcmp(instances[i]->target, s->target)) { err = ESP_ERR_INVALID_STATE; goto done; }
    }
    if (empty < EMOTION_LIMIT && next_owner != UINT32_MAX) {
        id = ++next_owner;
        instances[empty] = s; s->registered = true; err = ESP_OK;
    }
done:
    portEXIT_CRITICAL(&instances_lock);
    if (id) snprintf(s->owner, sizeof(s->owner), "emotion:%lu", (unsigned long)id);
    return err;
}
static void unregister_instance(solar_os_emotion_t *s)
{
    portENTER_CRITICAL(&instances_lock);
    for (unsigned i = 0; i < EMOTION_LIMIT; ++i) if (instances[i] == s) instances[i] = NULL;
    portEXIT_CRITICAL(&instances_lock);
    s->registered = false;
}

static void worker(void *arg)
{
    solar_os_emotion_t *s = arg;
    uint64_t next_frame = 0;
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    solar_os_audio_stream_t *audio = NULL;
    solar_os_audio_stream_format_t format = {0};
    uint32_t cue = 0, offset = 0;
    uint64_t cue_started = 0;
    int16_t samples[512];
#endif
    while (!atomic_load(&s->stop)) {
        uint64_t now = now_ms() - s->clock_offset;
        emotion_command_t command;
        /* Bounded work per iteration even when a producer continuously writes. */
        for (unsigned i = 0; i < EMOTION_QUEUE && xQueueReceive(s->commands, &command, 0); ++i) {
            int value = 0;
            switch (command.type) {
            case COMMAND_SET:
                value = s->api->set(s->state, command.pose, command.transition, now); break;
            case COMMAND_REACT:
                value = s->api->react(s->state, command.pose, command.hold, command.transition, now); break;
            case COMMAND_LOOK: value = s->api->look(s->state, command.x, command.y); break;
            case COMMAND_BLINK: value = s->api->blink(s->state, now); break;
            }
            if (value) atomic_store(&s->error, result(value));
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
            if (command.type == COMMAND_SET || command.type == COMMAND_REACT) {
                if (audio) { solar_os_audio_stream_close(audio); audio = NULL; }
                cue = command.cue; offset = 0; cue_started = now;
                if (cue && s->sound) {
                    esp_err_t err = solar_os_audio_stream_open(s->owner, 0, &audio, &format);
                    if (err == ESP_OK && (format.sample_format != SOLAR_OS_STREAM_AUDIO_S16_LE ||
                        format.bits_per_sample != 16 || !format.channels || format.channels > 2 ||
                        format.sample_rate < 8000 || format.sample_rate > 96000)) {
                        solar_os_audio_stream_close(audio); audio = NULL; err = ESP_ERR_NOT_SUPPORTED;
                    }
                    atomic_store(&s->audio_error, err);
                    if (err != ESP_OK) atomic_fetch_add(&s->audio_skipped, 1);
                }
            }
#endif
        }
        if (now >= next_frame) {
            if (atomic_exchange(&s->redraw, false)) s->have_frame = false;
            int value = s->api->render(s->state, now, s->pixels, s->bytes,
                s->width, s->height, s->stride, s->motion);
            if (value) atomic_store(&s->error, result(value));
            else if (!s->have_frame || memcmp(s->pixels, s->previous, s->bytes)) {
                bool accepted = false;
                if (s->software) {
                    solar_os_gfx_color_t palette[4];
                    bool inverted = solar_os_gfx_format(s->gfx) == SOLAR_OS_DISPLAY_FORMAT_MONO1 &&
                        solar_os_gfx_palette_inverted(s->gfx);
                    for (unsigned i = 0; i < 4; ++i) {
                        uint16_t c = s->palette[i];
                        palette[i] = inverted ? solar_os_gfx_rgb(255 - ((c >> 11) & 31) * 255 / 31,
                            255 - ((c >> 5) & 63) * 255 / 63, 255 - (c & 31) * 255 / 31) :
                            s->canvas_palette[i];
                    }
                    /* Row calls preserve INDEX2's padded stride on odd widths. */
                    for (unsigned y = 0; y < s->height; ++y)
                        solar_os_gfx_bitmap_2bpp(s->gfx, 0, y, s->width, 1,
                            s->pixels + (size_t)y * s->stride, s->stride, palette);
                    solar_os_gfx_present(s->gfx);
                    accepted = true;
                } else accepted = solar_os_frame_presenter_submit(s->presenter, s->pixels, s->bytes);
                if (accepted) {
                    memcpy(s->previous, s->pixels, s->bytes); s->have_frame = true;
                    atomic_fetch_add(&s->frames, 1);
                } else atomic_fetch_add(&s->dropped, 1);
            }
            if (s->presenter) {
                solar_os_frame_presenter_stats_t stats;
                solar_os_frame_presenter_take_stats(s->presenter, &stats);
                if (stats.last_error != ESP_OK) {
                    atomic_store(&s->error, stats.last_error); s->have_frame = false;
                }
            }
            next_frame = now + 1000U / s->fps;
        }
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
        if (audio) {
            uint32_t total = (uint64_t)s->api->cue_duration_ms(cue) * format.sample_rate / 1000;
            if (offset >= total || now - cue_started > s->api->cue_duration_ms(cue) + 100U) {
                solar_os_audio_stream_close(audio); audio = NULL;
            } else {
                size_t frames = total - offset;
                if (frames > 256) frames = 256;
                esp_err_t err = result(s->api->sound(cue, format.sample_rate, offset, samples, frames));
                if (format.channels == 2) for (size_t i = frames; i-- > 0;) {
                    samples[i * 2 + 1] = samples[i]; samples[i * 2] = samples[i];
                }
                if (err == ESP_OK) err = solar_os_audio_stream_write(audio, samples,
                    frames * format.channels * sizeof(*samples));
                offset += frames;
                if (err != ESP_OK) {
                    atomic_store(&s->audio_error, err);
                    solar_os_audio_stream_close(audio); audio = NULL;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(audio ? 1 : 10) ?: 1);
#else
        vTaskDelay(pdMS_TO_TICKS(10) ?: 1);
#endif
    }
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    if (audio) solar_os_audio_stream_close(audio);
#endif
    s->done = true;
    solar_os_task_delete_internal(NULL);
}
static esp_err_t start_worker(solar_os_emotion_t *s)
{
    atomic_store(&s->stop, false); s->done = false;
    uint32_t stack = 6144;
    solar_os_display_target_t target;
    if (s->software && solar_os_display_find_target(s->target, &target) &&
        target.presentation_stack_bytes > stack) stack = target.presentation_stack_bytes;
    if (solar_os_task_create_pinned_internal(worker, "emotion", stack, s,
        tskIDLE_PRIORITY + 1, &s->worker, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        s->worker = NULL; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
static esp_err_t stop_worker(solar_os_emotion_t *s)
{
    atomic_store(&s->stop, true);
    if (s->worker && !solar_os_task_wait_done(s->worker, &s->done, SOLAR_OS_TASK_STOP_WAIT_MS))
        return ESP_ERR_TIMEOUT;
    s->worker = NULL;
    return ESP_OK;
}
esp_err_t solar_os_emotion_close(solar_os_emotion_t **instance)
{
    if (!instance) return ESP_ERR_INVALID_ARG;
    solar_os_emotion_t *s = *instance;
    if (!s) return ESP_OK;
    esp_err_t err = stop_worker(s);
    if (err != ESP_OK) return err;
    /* Preserve everything on a timeout, including palette and library code. */
    err = solar_os_frame_presenter_suspend_checked(s->presenter);
    if (err != ESP_OK) return err;
    err = solar_os_frame_presenter_deinit(s->presenter);
    if (err != ESP_OK) return err;
    s->presenter = NULL;
    if (s->claimed) {
        err = solar_os_display_release(s->target, s->display_owner);
        if (err != ESP_OK) return err;
        s->claimed = false;
    }
    if (s->lease) {
        err = result(solar_os_native_library_release(s->lease));
        if (err != ESP_OK) return err;
        s->lease = 0;
    }
    if (s->registered) unregister_instance(s);
    if (s->commands) solar_os_queue_delete(s->commands);
    solar_os_memory_free(s->previous); solar_os_memory_free(s->pixels); solar_os_memory_free(s->state);
    solar_os_memory_free(s); *instance = NULL;
    return ESP_OK;
}
esp_err_t solar_os_emotion_open(const solar_os_emotion_options_t *o,
    solar_os_gfx_t *foreground, solar_os_emotion_t **out, char *detail, size_t detail_size)
{
    if (!o || !out || *out || o->fps > 30 || o->motion < -1 || o->motion > 1 ||
        (o->foreground_set && o->foreground_rgb888 > 0xffffffU) ||
        (o->background_set && o->background_rgb888 > 0xffffffU))
        return ESP_ERR_INVALID_ARG;
    if (detail && detail_size) detail[0] = 0;
    solar_os_emotion_t *s = solar_os_memory_calloc(1, sizeof(*s),
        SOLAR_OS_MEMORY_INTERNAL_CRITICAL, "emotion.runtime");
    if (!s) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    char current[SOLAR_OS_DISPLAY_TARGET_NAME_MAX] = {0};
    if (foreground) solar_os_gfx_display_target_name(foreground, current, sizeof(current));
    const char *name = o->display ? o->display : current;
    solar_os_display_target_t target;
    if (!*name || strlen(name) >= sizeof(s->target) || !solar_os_display_find_target(name, &target)) {
        if (detail && detail_size) snprintf(detail, detail_size, "no display; select a ready target from display list");
        goto fail;
    }
    s->software = target.frame_formats == 0;
    strlcpy(s->target, name, sizeof(s->target));
    err = register_instance(s);
    if (err != ESP_OK) {
        if (detail && detail_size) snprintf(detail, detail_size, "display already has an emotion instance or instance limit reached");
        goto fail;
    }
    if (foreground && !strcmp(name, current)) {
        strlcpy(s->display_owner, target.owner[0] ? target.owner : s->owner, sizeof(s->display_owner));
        err = solar_os_display_claim(name, s->display_owner, detail, detail_size);
        if (err != ESP_OK) goto fail;
        s->claimed = true; s->foreground = true; s->gfx = foreground;
    } else {
        strlcpy(s->display_owner, s->owner, sizeof(s->display_owner));
        err = solar_os_display_open_gfx(name, s->owner, &s->gfx, detail, detail_size);
        if (err != ESP_OK) goto fail;
        s->claimed = true;
    }
    const void *api = NULL;
    err = result(solar_os_native_library_acquire(SOLAR_OS_EMOTION_LIBRARY,
        SOLAR_OS_EMOTION_ABI, sizeof(*s->api), &s->lease, &api));
    if (err != ESP_OK) {
        if (detail && detail_size) snprintf(detail, detail_size, "install compatible emotion library first");
        goto fail;
    }
    s->api = api;
    if (!s->api->state_bytes || s->api->state_bytes > 16384 || !s->api->init ||
        !s->api->pose_find || !s->api->set || !s->api->react || !s->api->look ||
        !s->api->blink || !s->api->render || !s->api->sound || !s->api->cue_duration_ms) {
        err = ESP_ERR_NOT_SUPPORTED; goto fail;
    }
    s->width = solar_os_gfx_width(s->gfx); s->height = solar_os_gfx_height(s->gfx);
    if (!s->width || !s->height || s->width > 2048 || s->height > 2048) {
        err = ESP_ERR_NOT_SUPPORTED; goto fail;
    }
    s->stride = (s->width + 3U) / 4U; s->bytes = (size_t)s->stride * s->height;
    s->fps = o->fps ? o->fps : (target.preferred_stream_fps ? target.preferred_stream_fps : 10);
    if (s->fps > 25) s->fps = 25;
    if (target.preferred_stream_fps && s->fps > target.preferred_stream_fps) s->fps = target.preferred_stream_fps;
    s->motion = o->motion < 0 ? target.preferred_stream_fps >= 20 : o->motion != 0;
    if (!s->motion && s->fps > 2) s->fps = 2;
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    s->sound = o->sound && solar_os_audio_output_available();
#endif
    uint32_t colors[2] = {0, 0xffffffU};
    (void)solar_os_display_get_colors(&colors[0], &colors[1]);
    if (o->foreground_set) colors[0] = o->foreground_rgb888;
    if (o->background_set) colors[1] = o->background_rgb888;
    uint16_t endpoints[2];
    for (unsigned i = 0; i < 2; ++i)
        endpoints[i] = (uint16_t)(((colors[i] >> 8) & 0xf800U) |
            ((colors[i] >> 5) & 0x07e0U) | ((colors[i] >> 3) & 0x001fU));
    solar_os_emotion_palette_rgb565(endpoints[0], endpoints[1], s->palette);
    for (unsigned i = 0; i < 4; ++i) {
        uint16_t c = s->palette[i];
        s->canvas_palette[i] = solar_os_gfx_rgb(((c >> 11) & 31) * 255 / 31,
            ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31);
    }
    if (s->software) solar_os_gfx_prepare_surface(s->gfx);
    s->state = solar_os_memory_calloc(1, s->api->state_bytes, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "emotion.state");
    s->pixels = solar_os_memory_alloc(s->bytes, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "emotion.frame");
    s->previous = solar_os_memory_alloc(s->bytes, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "emotion.previous");
    s->commands = solar_os_queue_create(EMOTION_QUEUE, sizeof(emotion_command_t));
    if (!s->state || !s->pixels || !s->previous || !s->commands) { err = ESP_ERR_NO_MEM; goto fail; }
    err = result(s->api->init(s->state, now_ms(), (uint32_t)esp_timer_get_time()));
    if (err != ESP_OK) goto fail;
    const solar_os_frame_presenter_config_t config = {
        .gfx = s->gfx, .format = SOLAR_OS_DISPLAY_FORMAT_INDEX2,
        .width = s->width, .height = s->height, .stride = s->stride,
        .palette_rgb565 = s->palette, .palette_size = 4, .preferred_fps = s->fps,
        .allow_mono_fallback = true, .literal_direct_palette = true, .literal_mono_palette = true,
        .clear_background_on_resume = true, .background_index = 0,
    };
    err = s->software ? ESP_OK : solar_os_frame_presenter_init(&s->presenter, &config);
    if (err != ESP_OK) goto fail;
    err = start_worker(s);
    if (err != ESP_OK) goto fail;
    *out = s; return ESP_OK;
fail:
    /* Admission has not started our worker. A presenter that cannot stop is
     * still returned for cleanup; never orphan its callback data. */
    if (solar_os_emotion_close(&s) != ESP_OK) *out = s;
    return err;
}
esp_err_t solar_os_emotion_suspend(solar_os_emotion_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    if (!s->paused) { s->pause_started = now_ms(); s->paused = true; }
    esp_err_t err = stop_worker(s);
    if (err == ESP_OK) err = solar_os_frame_presenter_suspend_checked(s->presenter);
    if (err == ESP_OK && s->foreground && s->claimed) {
        err = solar_os_display_release(s->target, s->display_owner);
        if (err == ESP_OK) s->claimed = false;
    }
    return err;
}
esp_err_t solar_os_emotion_resume(solar_os_emotion_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    if (!s->paused) {
        if (s->presenter) (void)solar_os_frame_presenter_redraw(s->presenter);
        else atomic_store(&s->redraw, true);
        return ESP_OK;
    }
    if (s->worker) return ESP_ERR_INVALID_STATE;
    if (!s->claimed) {
        esp_err_t err = solar_os_display_claim(s->target, s->display_owner, NULL, 0);
        if (err != ESP_OK) { atomic_store(&s->error, err); return err; }
        s->claimed = true;
    }
    if (s->software) solar_os_gfx_prepare_surface(s->gfx);
    esp_err_t err = s->software ? ESP_OK : solar_os_frame_presenter_resume(s->presenter);
    if (err != ESP_OK) { atomic_store(&s->error, err); return err; }
    uint64_t elapsed = now_ms() - s->pause_started;
    s->clock_offset += elapsed;
    s->have_frame = false;
    err = start_worker(s);
    if (err != ESP_OK) { s->clock_offset -= elapsed; atomic_store(&s->error, err); return err; }
    s->paused = false; return ESP_OK;
}
static esp_err_t send(solar_os_emotion_t *s, const emotion_command_t *command)
{
    if (!s || (atomic_load(&s->stop) && !atomic_load(&s->paused))) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s->commands, command, 0) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}
esp_err_t solar_os_emotion_set(solar_os_emotion_t *s, const char *name, uint32_t transition)
{
    if (!s || transition > 60000) return ESP_ERR_INVALID_ARG;
    int pose = s->api->pose_find(name);
    if (pose < 0) return ESP_ERR_INVALID_ARG;
    emotion_command_t c = {.type = COMMAND_SET, .pose = pose, .transition = transition};
    return send(s, &c);
}
esp_err_t solar_os_emotion_react(solar_os_emotion_t *s, const char *name,
    uint32_t hold, uint32_t transition, const char *sound)
{
    if (!s || hold > 60000 || transition > 60000) return ESP_ERR_INVALID_ARG;
    int pose = s->api->pose_find(name);
    if (pose < 0) return ESP_ERR_INVALID_ARG;
    uint32_t cue = 0;
    static const char *const cues[] = {"silent", "chirp", "giggle", "sigh", "alert"};
    if (sound) {
        for (; cue < SOLAR_OS_EMOTION_CUE_COUNT && strcmp(sound, cues[cue]); ++cue) {}
        if (cue == SOLAR_OS_EMOTION_CUE_COUNT) return ESP_ERR_INVALID_ARG;
    }
    emotion_command_t c = {.type = COMMAND_REACT, .pose = pose, .hold = hold,
        .transition = transition, .cue = cue};
    return send(s, &c);
}
esp_err_t solar_os_emotion_look(solar_os_emotion_t *s, int32_t x, int32_t y)
{
    if (x < -1000 || x > 1000 || y < -1000 || y > 1000) return ESP_ERR_INVALID_ARG;
    emotion_command_t c = {.type = COMMAND_LOOK, .x = x, .y = y}; return send(s, &c);
}
esp_err_t solar_os_emotion_blink(solar_os_emotion_t *s)
{ emotion_command_t c = {.type = COMMAND_BLINK}; return send(s, &c); }
void solar_os_emotion_status(solar_os_emotion_t *s, solar_os_emotion_status_t *status)
{
    if (!s || !status) return;
    *status = (solar_os_emotion_status_t){.frames = atomic_load(&s->frames),
        .dropped = atomic_load(&s->dropped), .audio_skipped = atomic_load(&s->audio_skipped),
        .error = atomic_load(&s->error), .audio_error = atomic_load(&s->audio_error),
        .fps = s->fps, .width = s->width, .height = s->height,
        .sound = s->sound, .motion = s->motion};
}
bool solar_os_emotion_uses_gfx(const solar_os_emotion_t *s, const solar_os_gfx_t *gfx)
{ return s && s->gfx == gfx; }
