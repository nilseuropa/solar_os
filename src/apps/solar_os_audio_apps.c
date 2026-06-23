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
#include "solar_os_storage.h"
#include "solar_os_task.h"
#include "solar_os_terminal.h"

#define AUDIO_APP_TASK_STACK 28672
#define AUDIO_APP_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define AUDIO_APP_EVENT_QUEUE_LEN 8
#define AUDIO_APP_MESSAGE_MAX 96
#define AUDIO_APP_DEFAULT_RECORD_MS 10000U
#define AUDIO_APP_DEFAULT_VOLUME 50U

typedef enum {
    AUDIO_APP_MODE_RECORD,
    AUDIO_APP_MODE_PLAY,
} audio_app_mode_t;

typedef enum {
    AUDIO_APP_FORMAT_WAV,
    AUDIO_APP_FORMAT_MP3,
} audio_app_format_t;

typedef enum {
    AUDIO_APP_EVENT_STATUS,
    AUDIO_APP_EVENT_PROGRESS,
    AUDIO_APP_EVENT_ERROR,
    AUDIO_APP_EVENT_DONE,
} audio_app_event_type_t;

typedef struct {
    audio_app_event_type_t type;
    esp_err_t err;
    bool cancelled;
    solar_os_audio_wav_info_t info;
    char message[AUDIO_APP_MESSAGE_MAX];
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
static audio_app_state_t audio_app;

static solar_os_terminal_t *audio_app_terminal(solar_os_context_t *ctx)
{
    return solar_os_context_terminal(ctx);
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
    solar_os_terminal_t *term = audio_app_terminal(ctx);

    solar_os_terminal_clear(term);
    solar_os_terminal_writeln_bold(term, audio_app_name(mode));
    if (mode == AUDIO_APP_MODE_RECORD) {
        solar_os_terminal_writeln(term, "usage: arecord [-d seconds] file.wav");
    } else {
        solar_os_terminal_writeln(term, "usage: aplay [-v volume] file.wav|file.mp3");
    }
    solar_os_terminal_writeln(term, "format: 16000 Hz stereo 16-bit PCM WAV, MP3");
    solar_os_terminal_writeln(term, "CTRL+ALT+DEL exits");
}

static bool audio_app_parse_record_args(solar_os_context_t *ctx)
{
    audio_app.duration_ms = AUDIO_APP_DEFAULT_RECORD_MS;

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
    audio_app.volume = AUDIO_APP_DEFAULT_VOLUME;

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

static void audio_app_send_message(audio_app_event_type_t type, const char *message)
{
    audio_app_event_t event = {
        .type = type,
    };
    if (message != NULL) {
        strlcpy(event.message, message, sizeof(event.message));
    }
    (void)audio_app_send_event(&event);
}

static void audio_app_cleanup_resources(void)
{
    if (audio_app.events != NULL) {
        vQueueDelete(audio_app.events);
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

    if (audio_app.events != NULL) {
        (void)xQueueSend(audio_app.events, &event, pdMS_TO_TICKS(500));
    }
}

static void audio_app_task(void *arg)
{
    (void)arg;

    solar_os_audio_wav_info_t info = {0};
    solar_os_audio_wav_options_t options = {
        .should_cancel = audio_app_should_cancel,
        .progress = audio_app_progress,
        .user = NULL,
        .progress_interval_ms = SOLAR_OS_AUDIO_WAV_DEFAULT_PROGRESS_MS,
    };

    audio_app_send_message(AUDIO_APP_EVENT_STATUS,
                           audio_app.mode == AUDIO_APP_MODE_RECORD ? "recording" : "playing");

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
    vTaskDelete(NULL);
}

static void audio_app_print_info(solar_os_terminal_t *term,
                                 audio_app_format_t format,
                                 const solar_os_audio_wav_info_t *info)
{
    if (format == AUDIO_APP_FORMAT_MP3 && info->duration_ms == 0) {
        solar_os_terminal_printf(term,
                                 "MP3, %" PRIu32 " Hz, %u ch, %u bit\n",
                                 info->sample_rate,
                                 (unsigned)info->channels,
                                 (unsigned)info->bits_per_sample);
    } else {
        solar_os_terminal_printf(term,
                                 "%s, %" PRIu32 " Hz, %u ch, %u bit, %" PRIu32 " ms\n",
                                 format == AUDIO_APP_FORMAT_MP3 ? "MP3" : "WAV",
                                 info->sample_rate,
                                 (unsigned)info->channels,
                                 (unsigned)info->bits_per_sample,
                                 info->duration_ms);
    }
}

static esp_err_t audio_app_start_common(solar_os_context_t *ctx, audio_app_mode_t mode)
{
    solar_os_terminal_t *term = audio_app_terminal(ctx);

    if (audio_app.task != NULL && !audio_app.task_done) {
        solar_os_terminal_clear(term);
        solar_os_terminal_writeln(term, "audio: previous task is still stopping");
        solar_os_terminal_writeln(term, "CTRL+ALT+DEL exits");
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
        return ESP_OK;
    }

    solar_os_terminal_clear(term);
    solar_os_terminal_writeln_bold(term, audio_app_name(mode));
    solar_os_terminal_printf(term, "file: %s\n", audio_app.path);

    if (!solar_os_storage_is_mounted()) {
        solar_os_terminal_writeln(term, "SD card not mounted");
        solar_os_terminal_writeln(term, "CTRL+ALT+DEL exits");
        return ESP_OK;
    }

    if (mode == AUDIO_APP_MODE_RECORD) {
        solar_os_terminal_printf(term, "duration: %" PRIu32 " s\n", audio_app.duration_ms / 1000U);
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
                solar_os_terminal_printf(term,
                                         "aplay: open failed: %s\n",
                                         esp_err_to_name(mp3_err));
                solar_os_terminal_writeln(term, "CTRL+ALT+DEL exits");
                return ESP_OK;
            } else {
                solar_os_terminal_writeln(term, "aplay: unsupported audio file");
                solar_os_terminal_writeln(term, "CTRL+ALT+DEL exits");
                return ESP_OK;
            }
        }
        if (source.channels == 0 || source.sample_rate == 0 || source.bits_per_sample == 0) {
            solar_os_terminal_writeln(term, "aplay: unsupported audio file");
            solar_os_terminal_writeln(term, "CTRL+ALT+DEL exits");
            return ESP_OK;
        }
        solar_os_terminal_write(term, "source: ");
        audio_app_print_info(term, audio_app.format, &source);
        solar_os_terminal_printf(term, "volume: %u\n", (unsigned)audio_app.volume);
    }

    audio_app.events = xQueueCreate(AUDIO_APP_EVENT_QUEUE_LEN, sizeof(audio_app_event_t));
    if (audio_app.events == NULL) {
        solar_os_terminal_writeln(term, "audio: out of memory");
        solar_os_terminal_writeln(term, "CTRL+ALT+DEL exits");
        return ESP_OK;
    }

    audio_app.running = true;
    const BaseType_t created = xTaskCreatePinnedToCore(audio_app_task,
                                                       audio_app_name(mode),
                                                       AUDIO_APP_TASK_STACK,
                                                       NULL,
                                                       AUDIO_APP_TASK_PRIORITY,
                                                       &audio_app.task,
                                                       tskNO_AFFINITY);
    if (created != pdPASS) {
        vQueueDelete(audio_app.events);
        audio_app.events = NULL;
        audio_app.running = false;
        solar_os_terminal_writeln(term, "audio: task create failed");
        solar_os_terminal_writeln(term, "CTRL+ALT+DEL exits");
    }
    return ESP_OK;
}

static void audio_app_drain_events(solar_os_context_t *ctx)
{
    if (audio_app.events == NULL) {
        return;
    }

    solar_os_terminal_t *term = audio_app_terminal(ctx);
    audio_app_event_t event;
    while (xQueueReceive(audio_app.events, &event, 0) == pdPASS) {
        switch (event.type) {
        case AUDIO_APP_EVENT_STATUS:
            solar_os_terminal_printf(term, "%s: %s\n", audio_app_name(audio_app.mode), event.message);
            break;
        case AUDIO_APP_EVENT_PROGRESS:
            solar_os_terminal_printf(term,
                                     "%s: %" PRIu32 " bytes, %" PRIu32 " ms\n",
                                     audio_app_name(audio_app.mode),
                                     event.info.data_bytes,
                                     event.info.duration_ms);
            break;
        case AUDIO_APP_EVENT_ERROR:
            solar_os_terminal_printf(term, "%s: %s\n", audio_app_name(audio_app.mode), event.message);
            break;
        case AUDIO_APP_EVENT_DONE:
            audio_app.running = false;
            audio_app.done = true;
            if (event.cancelled) {
                solar_os_terminal_printf(term,
                                         "%s: stopped, %" PRIu32 " bytes, %" PRIu32 " ms\n",
                                         audio_app_name(audio_app.mode),
                                         event.info.data_bytes,
                                         event.info.duration_ms);
            } else if (event.err == ESP_OK) {
                solar_os_terminal_printf(term,
                                         "%s: done, %" PRIu32 " bytes, %" PRIu32 " ms\n",
                                         audio_app_name(audio_app.mode),
                                         event.info.data_bytes,
                                         event.info.duration_ms);
            } else if (event.err == ESP_ERR_NOT_SUPPORTED) {
                solar_os_terminal_printf(term,
                                         "%s: unsupported audio format\n",
                                         audio_app_name(audio_app.mode));
            } else {
                solar_os_terminal_printf(term,
                                         "%s failed: %s\n",
                                         audio_app_name(audio_app.mode),
                                         esp_err_to_name(event.err));
            }
            solar_os_terminal_writeln(term, "CTRL+ALT+DEL exits");
            break;
        default:
            break;
        }
    }
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
    if (ch == SOLAR_OS_KEY_APP_EXIT) {
        if (audio_app.running) {
            solar_os_terminal_printf(audio_app_terminal(ctx),
                                     "\n%s: stopping\n",
                                     audio_app_name(audio_app.mode));
        }
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (ch == SOLAR_OS_KEY_PAGE_UP) {
        solar_os_terminal_page_up(audio_app_terminal(ctx));
        return true;
    }
    if (ch == SOLAR_OS_KEY_PAGE_DOWN) {
        solar_os_terminal_page_down(audio_app_terminal(ctx));
        return true;
    }
    return true;
}

const solar_os_app_t solar_os_arecord_app = {
    .name = "arecord",
    .summary = "record WAV audio",
    .start = arecord_start,
    .stop = audio_app_stop,
    .event = audio_app_event,
};

const solar_os_app_t solar_os_aplay_app = {
    .name = "aplay",
    .summary = "play WAV/MP3 audio",
    .start = aplay_start,
    .stop = audio_app_stop,
    .event = audio_app_event,
};
