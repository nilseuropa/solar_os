#include "solar_os_audio_apps.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "solar_os_audio.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_queue.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"
#include "solar_os_terminal.h"

#define AUDIO_APP_TASK_STACK 28672
#define AUDIO_APP_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(AUDIO_APP_TASK_STACK);
#define AUDIO_APP_EVENT_QUEUE_LEN 8

typedef enum {
    AUDIO_APP_MODE_RECORD,
    AUDIO_APP_MODE_PLAY,
} audio_app_mode_t;

typedef enum {
    AUDIO_APP_FORMAT_WAV,
    AUDIO_APP_FORMAT_MP3,
} audio_app_format_t;

typedef enum {
    AUDIO_APP_EVENT_PROGRESS,
    AUDIO_APP_EVENT_DONE,
} audio_app_event_type_t;

typedef struct {
    audio_app_event_type_t type;
    esp_err_t err;
    bool cancelled;
    solar_os_audio_wav_info_t info;
} audio_app_event_t;

typedef struct {
    audio_app_mode_t mode;
    audio_app_format_t format;
    QueueHandle_t events;
    TaskHandle_t task;
    volatile bool stop_requested;
    volatile bool task_done;
    bool running;
    bool done;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    uint32_t duration_ms;
    uint8_t volume;
} audio_app_state_t;

static const char *TAG = "solar_os_audio_app";
typedef struct {
    audio_app_state_t app;
    solar_os_shell_io_t fallback_io;
} audio_app_cold_state_t;

static void *audio_app_state;
#define audio_app (((audio_app_cold_state_t *)audio_app_state)->app)
#define audio_app_fallback_io \
    (((audio_app_cold_state_t *)audio_app_state)->fallback_io)

static bool audio_app_uses_external_worker(void)
{
    return audio_app.mode == AUDIO_APP_MODE_PLAY;
}

static solar_os_shell_io_t *audio_app_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&audio_app_fallback_io,
                                        solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &audio_app_fallback_io);
        io = &audio_app_fallback_io;
    }
    return io;
}

static void audio_app_request_close(solar_os_context_t *ctx,
                                    int exit_code,
                                    const char *message)
{
    solar_os_context_finish(ctx, exit_code, message);
}

static const char *audio_app_name(audio_app_mode_t mode)
{
    return mode == AUDIO_APP_MODE_RECORD ? "arecord" : "aplay";
}

static bool audio_app_parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static bool audio_app_parse_u8(const char *text, uint8_t *value)
{
    uint32_t parsed = 0;

    if (!audio_app_parse_u32(text, 0, 100, &parsed)) {
        return false;
    }

    *value = (uint8_t)parsed;
    return true;
}

static void audio_app_render_usage(solar_os_context_t *ctx, audio_app_mode_t mode)
{
    solar_os_shell_io_t *io = audio_app_io(ctx);

    if (mode == AUDIO_APP_MODE_RECORD) {
        solar_os_shell_io_writeln(io, "usage: arecord [-d seconds] file.wav");
    } else {
        solar_os_shell_io_writeln(io, "usage: aplay [-v volume] file.wav|file.mp3");
    }
    solar_os_shell_io_writeln(io, "formats: 16-bit PCM WAV, MP3");
    solar_os_shell_io_flush(io);
}

static bool audio_app_parse_record_args(solar_os_context_t *ctx)
{
    audio_app.duration_ms = 0U;

    const int argc = solar_os_context_argc(ctx);
    for (int i = 1; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (strcmp(arg, "-d") == 0) {
            uint32_t seconds = 0;
            if (i + 1 >= argc ||
                !audio_app_parse_u32(solar_os_context_argv(ctx, i + 1),
                                     1,
                                     SOLAR_OS_AUDIO_WAV_MAX_MS / 1000U,
                                     &seconds)) {
                return false;
            }
            audio_app.duration_ms = seconds * 1000U;
            i++;
            continue;
        }
        if (arg[0] == '-' || audio_app.path[0] != '\0') {
            return false;
        }
        if (solar_os_storage_resolve_path(arg, audio_app.path, sizeof(audio_app.path)) != ESP_OK) {
            return false;
        }
    }

    return audio_app.path[0] != '\0';
}

static bool audio_app_parse_play_args(solar_os_context_t *ctx)
{
    audio_app.volume = SOLAR_OS_AUDIO_VOLUME_GLOBAL;

    const int argc = solar_os_context_argc(ctx);
    for (int i = 1; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (strcmp(arg, "-v") == 0) {
            if (i + 1 >= argc ||
                !audio_app_parse_u8(solar_os_context_argv(ctx, i + 1), &audio_app.volume)) {
                return false;
            }
            i++;
            continue;
        }
        if (arg[0] == '-' || audio_app.path[0] != '\0') {
            return false;
        }
        if (solar_os_storage_resolve_path(arg, audio_app.path, sizeof(audio_app.path)) != ESP_OK) {
            return false;
        }
    }

    return audio_app.path[0] != '\0';
}

static bool audio_app_send_event(const audio_app_event_t *event)
{
    if (event == NULL || audio_app.events == NULL) {
        return false;
    }

    while (!audio_app.stop_requested) {
        if (xQueueSend(audio_app.events, event, pdMS_TO_TICKS(100)) == pdPASS) {
            return true;
        }
    }
    return false;
}

static void audio_app_cleanup_resources(void)
{
    if (audio_app.events != NULL) {
        if (audio_app_uses_external_worker()) {
            solar_os_queue_delete(audio_app.events);
        } else {
            solar_os_queue_delete_internal(audio_app.events);
        }
        audio_app.events = NULL;
    }
}

static bool audio_app_should_cancel(void *user)
{
    (void)user;

    return audio_app.stop_requested;
}

static void audio_app_progress(const solar_os_audio_wav_progress_t *progress, void *user)
{
    (void)user;

    if (progress == NULL || progress->done) {
        return;
    }

    audio_app_event_t event = {
        .type = AUDIO_APP_EVENT_PROGRESS,
        .info = progress->info,
    };
    (void)audio_app_send_event(&event);
}

static void audio_app_send_done(esp_err_t err,
                                bool cancelled,
                                const solar_os_audio_wav_info_t *info)
{
    audio_app_event_t event = {
        .type = AUDIO_APP_EVENT_DONE,
        .err = err,
        .cancelled = cancelled,
    };
    if (info != NULL) {
        event.info = *info;
    }

    while (audio_app.events != NULL && !audio_app.stop_requested) {
        if (xQueueSend(audio_app.events, &event, pdMS_TO_TICKS(100)) == pdPASS) {
            break;
        }
    }
}

static void audio_app_task(void *arg)
{
    (void)arg;

    solar_os_audio_wav_info_t info = {0};
    solar_os_audio_wav_options_t options = {
        .should_cancel = audio_app_should_cancel,
        .progress = audio_app.mode == AUDIO_APP_MODE_RECORD ?
            audio_app_progress : NULL,
        .user = NULL,
        .progress_interval_ms = SOLAR_OS_AUDIO_WAV_DEFAULT_PROGRESS_MS,
    };

    esp_err_t err;
    if (audio_app.mode == AUDIO_APP_MODE_RECORD) {
        err = solar_os_audio_record_wav(audio_app.path,
                                        audio_app.duration_ms,
                                        &options,
                                        &info);
    } else if (audio_app.format == AUDIO_APP_FORMAT_MP3) {
        err = solar_os_audio_play_mp3(audio_app.path, audio_app.volume, &options, &info);
    } else {
        err = solar_os_audio_play_wav(audio_app.path, audio_app.volume, &options, &info);
    }

    const bool cancelled = audio_app.stop_requested || err == ESP_ERR_TIMEOUT;
    audio_app_send_done(err, cancelled, &info);
    SOLAR_OS_LOGI(TAG,
             "%s done path=%s bytes=%" PRIu32 " ms=%" PRIu32 " ret=%s",
             audio_app_name(audio_app.mode),
             audio_app.path,
             info.data_bytes,
             info.duration_ms,
             esp_err_to_name(err));
    audio_app.task_done = true;
    if (audio_app_uses_external_worker()) {
        solar_os_task_delete_external(NULL);
    } else {
        solar_os_task_delete_internal(NULL);
    }
}

static void audio_app_print_info(solar_os_shell_io_t *io,
                                 audio_app_format_t format,
                                 const solar_os_audio_wav_info_t *info)
{
    if (format == AUDIO_APP_FORMAT_MP3 && info->duration_ms == 0) {
        solar_os_shell_io_printf(io,
                                 "MP3, %" PRIu32 " Hz, %u ch, %u bit",
                                 info->sample_rate,
                                 (unsigned)info->channels,
                                 (unsigned)info->bits_per_sample);
    } else {
        solar_os_shell_io_printf(io,
                                 "%s, %" PRIu32 " Hz, %u ch, %u bit, %" PRIu32 " ms",
                                 format == AUDIO_APP_FORMAT_MP3 ? "MP3" : "WAV",
                                 info->sample_rate,
                                 (unsigned)info->channels,
                                 (unsigned)info->bits_per_sample,
                                 info->duration_ms);
    }
}

static esp_err_t audio_app_start_common(solar_os_context_t *ctx, audio_app_mode_t mode)
{
    solar_os_shell_io_t *io = audio_app_io(ctx);

    if (audio_app.task != NULL && !audio_app.task_done) {
        solar_os_shell_io_writeln(io, "audio: previous task is still stopping");
        solar_os_shell_io_flush(io);
        audio_app_request_close(ctx, 1, "audio: previous task is still stopping");
        return ESP_OK;
    }

    audio_app_cleanup_resources();
    memset(&audio_app, 0, sizeof(audio_app));
    audio_app.mode = mode;

    const bool parsed = mode == AUDIO_APP_MODE_RECORD ?
        audio_app_parse_record_args(ctx) :
        audio_app_parse_play_args(ctx);
    if (!parsed) {
        audio_app_render_usage(ctx, mode);
        audio_app_request_close(
            ctx,
            2,
            mode == AUDIO_APP_MODE_RECORD ?
                "usage: arecord [-d seconds] <file.wav>" :
                "usage: aplay [-v volume] <file.wav|file.mp3>");
        return ESP_OK;
    }

    if (!solar_os_storage_is_mounted()) {
        solar_os_shell_io_writeln(io, "audio: storage not mounted");
        solar_os_shell_io_flush(io);
        audio_app_request_close(ctx, 1, "audio: storage not mounted");
        return ESP_OK;
    }

    if (mode == AUDIO_APP_MODE_RECORD) {
        if (audio_app.duration_ms == 0U) {
            solar_os_shell_io_printf(io,
                                     "recording %s until stopped\n",
                                     audio_app.path);
        } else {
            solar_os_shell_io_printf(io,
                                     "recording %s for %" PRIu32 " s\n",
                                     audio_app.path,
                                     audio_app.duration_ms / 1000U);
        }
    } else {
        solar_os_audio_wav_info_t source;
        const esp_err_t wav_err = solar_os_audio_get_wav_info(audio_app.path, &source);
        if (wav_err == ESP_OK) {
            audio_app.format = AUDIO_APP_FORMAT_WAV;
        } else {
            const esp_err_t mp3_err = solar_os_audio_get_mp3_info(audio_app.path, &source);
            if (mp3_err == ESP_OK) {
                audio_app.format = AUDIO_APP_FORMAT_MP3;
            } else if (wav_err == ESP_FAIL || mp3_err == ESP_FAIL) {
                solar_os_shell_io_printf(io,
                                         "aplay: open failed: %s\n",
                                         esp_err_to_name(mp3_err));
                solar_os_shell_io_flush(io);
                char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
                snprintf(message,
                         sizeof(message),
                         "aplay: open failed: %s",
                         esp_err_to_name(mp3_err));
                audio_app_request_close(ctx, 1, message);
                return ESP_OK;
            } else {
                solar_os_shell_io_writeln(io, "aplay: unsupported audio file");
                solar_os_shell_io_flush(io);
                audio_app_request_close(ctx, 1, "aplay: unsupported audio file");
                return ESP_OK;
            }
        }
        if (source.channels == 0 || source.sample_rate == 0 || source.bits_per_sample == 0) {
            solar_os_shell_io_writeln(io, "aplay: unsupported audio file");
            solar_os_shell_io_flush(io);
            audio_app_request_close(ctx, 1, "aplay: unsupported audio file");
            return ESP_OK;
        }
        solar_os_shell_io_printf(io, "playing %s (", audio_app.path);
        audio_app_print_info(io, audio_app.format, &source);
        if (audio_app.volume != SOLAR_OS_AUDIO_VOLUME_GLOBAL) {
            solar_os_shell_io_printf(io,
                                     ", volume %u%%",
                                     (unsigned)audio_app.volume);
        }
        solar_os_shell_io_writeln(io, ")");
    }
    solar_os_shell_io_flush(io);

    audio_app.events = audio_app_uses_external_worker() ?
        solar_os_queue_create(AUDIO_APP_EVENT_QUEUE_LEN,
                              sizeof(audio_app_event_t)) :
        solar_os_queue_create_internal(AUDIO_APP_EVENT_QUEUE_LEN,
                                       sizeof(audio_app_event_t));
    if (audio_app.events == NULL) {
        solar_os_shell_io_writeln(io, "audio: out of memory");
        solar_os_shell_io_flush(io);
        audio_app_request_close(ctx, 1, "audio: out of memory");
        return ESP_OK;
    }

    audio_app.running = true;
    const BaseType_t created = audio_app_uses_external_worker() ?
        solar_os_task_create_pinned_external(
            audio_app_task,
            audio_app_name(mode),
            AUDIO_APP_TASK_STACK,
            NULL,
            AUDIO_APP_TASK_PRIORITY,
            &audio_app.task,
            tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_FOREGROUND) :
        solar_os_task_create_pinned_internal(
            audio_app_task,
            audio_app_name(mode),
            AUDIO_APP_TASK_STACK,
            NULL,
            AUDIO_APP_TASK_PRIORITY,
            &audio_app.task,
            tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        audio_app_cleanup_resources();
        audio_app.running = false;
        solar_os_shell_io_writeln(io, "audio: task create failed");
        solar_os_shell_io_flush(io);
        audio_app_request_close(ctx, 1, "audio: task create failed");
    }
    return ESP_OK;
}

static void audio_app_drain_events(solar_os_context_t *ctx)
{
    if (audio_app.events == NULL) {
        return;
    }

    solar_os_shell_io_t *io = audio_app_io(ctx);
    audio_app_event_t event;
    while (xQueueReceive(audio_app.events, &event, 0) == pdPASS) {
        switch (event.type) {
        case AUDIO_APP_EVENT_PROGRESS:
            if (audio_app.mode == AUDIO_APP_MODE_RECORD) {
                solar_os_shell_io_printf(io,
                                         "arecord: %" PRIu32 " bytes, %" PRIu32 " ms\n",
                                         event.info.data_bytes,
                                         event.info.duration_ms);
            }
            break;
        case AUDIO_APP_EVENT_DONE:
            audio_app.running = false;
            audio_app.done = true;
            if (audio_app.mode == AUDIO_APP_MODE_RECORD &&
                (event.cancelled || event.err == ESP_OK)) {
                solar_os_shell_io_printf(io,
                                         "arecord: %s, %" PRIu32 " bytes, %" PRIu32 " ms\n",
                                         event.cancelled ? "stopped" : "done",
                                         event.info.data_bytes,
                                         event.info.duration_ms);
            } else if (event.err == ESP_ERR_NOT_SUPPORTED) {
                solar_os_shell_io_printf(io,
                                         "%s: unsupported audio format\n",
                                         audio_app_name(audio_app.mode));
            } else if (event.err == ESP_ERR_NOT_FOUND) {
                solar_os_shell_io_printf(io,
                                         "%s: no audio %s device\n",
                                         audio_app_name(audio_app.mode),
                                         audio_app.mode == AUDIO_APP_MODE_RECORD ?
                                             "input" : "output");
            } else if (!event.cancelled && event.err != ESP_OK) {
                solar_os_shell_io_printf(io,
                                         "%s failed: %s\n",
                                         audio_app_name(audio_app.mode),
                                         esp_err_to_name(event.err));
            }
            solar_os_shell_io_flush(io);
            char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
            if (event.cancelled) {
                snprintf(message,
                         sizeof(message),
                         "%s: cancelled",
                         audio_app_name(audio_app.mode));
                audio_app_request_close(ctx, 130, message);
            } else if (event.err != ESP_OK) {
                snprintf(message,
                         sizeof(message),
                         "%s: %s",
                         audio_app_name(audio_app.mode),
                         esp_err_to_name(event.err));
                audio_app_request_close(ctx, 1, message);
            } else if (audio_app.mode == AUDIO_APP_MODE_RECORD) {
                snprintf(message,
                         sizeof(message),
                         "arecord: done, %" PRIu32 " bytes, %" PRIu32 " ms",
                         event.info.data_bytes,
                         event.info.duration_ms);
                audio_app_request_close(ctx, 0, message);
            } else {
                audio_app_request_close(ctx, 0, "aplay: done");
            }
            break;
        default:
            break;
        }
    }
    solar_os_shell_io_flush(io);
}

static esp_err_t arecord_start(solar_os_context_t *ctx)
{
    return audio_app_start_common(ctx, AUDIO_APP_MODE_RECORD);
}

static esp_err_t aplay_start(solar_os_context_t *ctx)
{
    return audio_app_start_common(ctx, AUDIO_APP_MODE_PLAY);
}

static void audio_app_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    audio_app.stop_requested = true;
    if (!solar_os_task_wait_done(audio_app.task,
                                 &audio_app.task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "audio task did not stop within %u ms",
                 (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        return;
    }

    audio_app_cleanup_resources();
    memset(&audio_app, 0, sizeof(audio_app));
}

static bool audio_app_state_release_ready(void)
{
    return audio_app.task == NULL || audio_app.task_done;
}

static bool audio_app_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        audio_app_drain_events(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT ||
        (audio_app.mode == AUDIO_APP_MODE_PLAY &&
         ch == SOLAR_OS_KEY_ESCAPE)) {
        if (audio_app.running && audio_app.mode == AUDIO_APP_MODE_RECORD) {
            solar_os_shell_io_t *io = audio_app_io(ctx);
            solar_os_shell_io_writeln(io, "\narecord: stopping");
            solar_os_shell_io_flush(io);
        }
        audio_app_request_close(
            ctx,
            audio_app.running ? 130 : 0,
            audio_app.running ? "audio: cancelled" : NULL);
        return true;
    }
    if (ch == SOLAR_OS_KEY_PAGE_UP) {
        solar_os_terminal_t *term = solar_os_context_terminal(ctx);
        if (term != NULL) {
            solar_os_terminal_page_up(term);
        }
        return true;
    }
    if (ch == SOLAR_OS_KEY_PAGE_DOWN) {
        solar_os_terminal_t *term = solar_os_context_terminal(ctx);
        if (term != NULL) {
            solar_os_terminal_page_down(term);
        }
        return true;
    }
    return true;
}

#if SOLAR_OS_PACKAGE_APP_ARECORD
const solar_os_app_t solar_os_arecord_app = {
    .name = "arecord",
    .summary = "record WAV audio",
    .app_class = SOLAR_OS_APP_CLASS_COMMAND,
    .start = arecord_start,
    .stop = audio_app_stop,
    .event = audio_app_event,
    .state_slot = &audio_app_state,
    .state_size = sizeof(audio_app_cold_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .state_release_ready = audio_app_state_release_ready,
    .state_release_cleanup = audio_app_cleanup_resources,
    .worker_stack_bytes = AUDIO_APP_TASK_STACK,
};
#endif

#if SOLAR_OS_PACKAGE_APP_APLAY
const solar_os_app_t solar_os_aplay_app = {
    .name = "aplay",
    .summary = "play WAV/MP3 audio",
    .app_class = SOLAR_OS_APP_CLASS_COMMAND,
    .start = aplay_start,
    .stop = audio_app_stop,
    .event = audio_app_event,
    .state_slot = &audio_app_state,
    .state_size = sizeof(audio_app_cold_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .state_release_ready = audio_app_state_release_ready,
    .state_release_cleanup = audio_app_cleanup_resources,
    .worker_stack_bytes = AUDIO_APP_TASK_STACK,
    .worker_stack_external = true,
};
#endif
