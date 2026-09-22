#include "solar_os_speechd_job.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "picotts.h"
#include "solar_os_audio.h"
#include "solar_os_audio_pcm.h"
#include "solar_os_audio_player.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_shell_io.h"
#include "solar_os_speech.h"
#include "solar_os_task.h"

#define SPEECHD_DISPATCH_STACK 4096U
#define SPEECHD_PICOTTS_STACK 8192U
#define SPEECHD_TOTAL_STACK (SPEECHD_DISPATCH_STACK + SPEECHD_PICOTTS_STACK)
#define SPEECHD_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define SPEECHD_AUDIO_WAIT_MS 100U
#define SPEECHD_AUDIO_BUFFER_BYTES (16U * 1024U)
#define SPEECHD_AUDIO_INTERNAL_BUFFER_BYTES (8U * 1024U)
#define SPEECHD_AUDIO_TARGET_MS 100U
#define SPEECHD_OUTPUT_SAMPLES 1024U
#define SPEECHD_STOP_WAIT_MS 5000U

typedef struct {
    TaskHandle_t task;
    SemaphoreHandle_t engine_event;
    StaticSemaphore_t engine_event_storage;
    solar_os_audio_player_t *player;
    solar_os_stream_audio_format_t output_format;
    solar_os_audio_s16_converter_t converter;
    int16_t converted[SPEECHD_OUTPUT_SAMPLES];
    volatile bool ready;
    volatile bool done;
    volatile bool stop_requested;
    volatile bool awaiting_idle;
    volatile bool engine_idle;
    volatile bool engine_failed;
    volatile esp_err_t output_error;
    bool engine_initialized;
    uint32_t current_id;
    uint32_t generation;
    esp_err_t last_error;
} speechd_state_t;

static EXT_RAM_BSS_ATTR speechd_state_t speechd;
static const char *TAG = "solar_os_speechd";

static const char *speechd_voice(void)
{
#if CONFIG_PICOTTS_LANGUAGE_EN_US
    return "en-US";
#elif CONFIG_PICOTTS_LANGUAGE_DE_DE
    return "de-DE";
#elif CONFIG_PICOTTS_LANGUAGE_ES_ES
    return "es-ES";
#elif CONFIG_PICOTTS_LANGUAGE_FR_FR
    return "fr-FR";
#elif CONFIG_PICOTTS_LANGUAGE_IT_IT
    return "it-IT";
#else
    return "en-GB";
#endif
}

static void speechd_output(int16_t *samples, unsigned count)
{
    if (samples == NULL || count == 0U || speechd.player == NULL ||
        speechd.stop_requested || speechd.current_id == 0U) {
        return;
    }
    const volatile bool *cancelled =
        solar_os_speech_worker_cancel_flag(speechd.current_id);
    if ((cancelled != NULL && *cancelled) || speechd.output_error != ESP_OK) {
        return;
    }

    const solar_os_stream_audio_format_t source = {
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
        .sample_rate = PICOTTS_SAMPLE_FREQ_HZ,
        .channels = 1U,
        .bits_per_sample = PICOTTS_SAMPLE_BITS,
    };
    bool source_done = false;
    while (!source_done && speechd.output_error == ESP_OK) {
        size_t output_samples = 0U;
        esp_err_t err = solar_os_audio_s16_convert(
            &speechd.converter,
            samples,
            count,
            &source,
            &speechd.output_format,
            speechd.converted,
            SPEECHD_OUTPUT_SAMPLES,
            &output_samples,
            &source_done);
        if (err == ESP_OK) {
            err = solar_os_audio_player_write(
                speechd.player,
                speechd.converted,
                output_samples * sizeof(speechd.converted[0]),
                cancelled);
        }
        if (err != ESP_OK) {
            speechd.output_error = err;
        }
    }
}

static void speechd_idle(void)
{
    if (!speechd.awaiting_idle) {
        return;
    }
    speechd.engine_idle = true;
    speechd.awaiting_idle = false;
    if (speechd.engine_event != NULL) {
        (void)xSemaphoreGive(speechd.engine_event);
    }
}

static void speechd_error(void)
{
    speechd.engine_failed = true;
    speechd.last_error = ESP_FAIL;
    if (speechd.engine_event != NULL) {
        (void)xSemaphoreGive(speechd.engine_event);
    }
    if (speechd.task != NULL) {
        xTaskNotifyGive(speechd.task);
    }
}

static esp_err_t speechd_open_player(const solar_os_speech_work_t *work)
{
    if (work == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const volatile bool *cancelled =
        solar_os_speech_worker_cancel_flag(work->id);
    do {
        const solar_os_audio_player_options_t options = {
            .owner = "job:speechd",
            .requested_audio = {
                .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
                .bits_per_sample = 16U,
            },
            .volume = work->volume,
            .buffered = true,
            .external_buffer_bytes = SPEECHD_AUDIO_BUFFER_BYTES,
            .internal_buffer_bytes = SPEECHD_AUDIO_INTERNAL_BUFFER_BYTES,
            .target_ms = SPEECHD_AUDIO_TARGET_MS,
            .open_timeout_ms = work->drop_if_busy ? 1U : SPEECHD_AUDIO_WAIT_MS,
        };
        const esp_err_t err = solar_os_audio_player_create(
            &options, &speechd.player, &speechd.output_format, NULL);
        if (err == ESP_OK) {
            solar_os_audio_s16_converter_reset(&speechd.converter);
            return ESP_OK;
        }
        if (work->drop_if_busy) {
            return ESP_ERR_INVALID_STATE;
        }
        if (err != ESP_ERR_INVALID_STATE) {
            return err;
        }
    } while (!speechd.stop_requested &&
             (cancelled == NULL || !*cancelled));
    return ESP_ERR_TIMEOUT;
}

static void speechd_close_player(const volatile bool *cancelled)
{
    if (speechd.player == NULL) {
        return;
    }
    if (speechd.output_error == ESP_OK) {
        const esp_err_t err = solar_os_audio_player_finish(
            speechd.player, cancelled);
        if (err != ESP_OK) {
            speechd.output_error = err;
        }
    }
    solar_os_audio_player_destroy(speechd.player);
    speechd.player = NULL;
}

static void speechd_drain_engine_event(void)
{
    while (xSemaphoreTake(speechd.engine_event, 0) == pdTRUE) {
    }
}

static void speechd_run_request(const solar_os_speech_work_t *work)
{
    speechd.current_id = work->id;
    speechd.output_error = ESP_OK;

    esp_err_t err = speechd_open_player(work);
    if (err != ESP_OK) {
        const solar_os_speech_request_state_t state =
            work->drop_if_busy && err == ESP_ERR_INVALID_STATE ?
                SOLAR_OS_SPEECH_REQUEST_DROPPED :
                (err == ESP_ERR_TIMEOUT ? SOLAR_OS_SPEECH_REQUEST_CANCELLED :
                                          SOLAR_OS_SPEECH_REQUEST_FAILED);
        (void)solar_os_speech_worker_finish(work->id, state, err);
        speechd.current_id = 0U;
        return;
    }

    (void)solar_os_speech_worker_set_state(
        work->id, SOLAR_OS_SPEECH_REQUEST_SPEAKING);
    speechd_drain_engine_event();
    speechd.engine_idle = false;
    speechd.awaiting_idle = false;
    picotts_add(work->text, strlen(work->text) + 1U);
    speechd.awaiting_idle = true;

    while (!speechd.engine_idle && !speechd.engine_failed &&
           !speechd.stop_requested) {
        (void)xSemaphoreTake(speechd.engine_event, pdMS_TO_TICKS(50U));
    }

    const volatile bool *cancelled =
        solar_os_speech_worker_cancel_flag(work->id);
    speechd_close_player(cancelled);

    solar_os_speech_request_state_t state = SOLAR_OS_SPEECH_REQUEST_COMPLETE;
    err = speechd.output_error;
    if (cancelled != NULL && *cancelled) {
        state = SOLAR_OS_SPEECH_REQUEST_CANCELLED;
        err = ESP_ERR_TIMEOUT;
    } else if (speechd.engine_failed || err != ESP_OK) {
        state = SOLAR_OS_SPEECH_REQUEST_FAILED;
        if (err == ESP_OK) {
            err = ESP_FAIL;
        }
    }
    (void)solar_os_speech_worker_finish(work->id, state, err);
    speechd.last_error = state == SOLAR_OS_SPEECH_REQUEST_FAILED ? err : ESP_OK;
    speechd.current_id = 0U;
}

static void speechd_task(void *arg)
{
    (void)arg;

    speechd.last_error = solar_os_speech_worker_start(xTaskGetCurrentTaskHandle());
    speechd.ready = true;
    if (speechd.last_error == ESP_OK) {
        while (!speechd.stop_requested && !speechd.engine_failed) {
            solar_os_speech_work_t work;
            const esp_err_t err = solar_os_speech_worker_take(
                &work, UINT32_MAX);
            if (err == ESP_OK) {
                speechd_run_request(&work);
            } else if (err != ESP_ERR_INVALID_STATE) {
                speechd.last_error = err;
            }
        }
    }

    solar_os_speech_worker_stop();
    speechd.done = true;
    if (!speechd.stop_requested && speechd.generation != 0U) {
        (void)solar_os_jobs_mark_stopped(solar_os_speechd_job.name,
                                         speechd.generation,
                                         speechd.last_error != ESP_OK ?
                                             speechd.last_error : ESP_FAIL);
    }
    solar_os_task_delete_internal(NULL);
}

static esp_err_t speechd_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc > 1 || (argc == 1 && argv != NULL && argv[0] != NULL &&
                     strcmp(argv[0], solar_os_speechd_job.name) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_audio_output_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (speechd.task != NULL || speechd.engine_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (speechd.engine_event == NULL) {
        speechd.engine_event = xSemaphoreCreateBinaryStatic(
            &speechd.engine_event_storage);
    }
    if (speechd.engine_event == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&speechd.converter, 0, sizeof(speechd.converter));
    speechd.ready = false;
    speechd.done = false;
    speechd.stop_requested = false;
    speechd.awaiting_idle = false;
    speechd.engine_idle = true;
    speechd.engine_failed = false;
    speechd.output_error = ESP_OK;
    speechd.last_error = ESP_OK;
    speechd.current_id = 0U;
    esp_err_t err = solar_os_jobs_get_generation(
        solar_os_speechd_job.name, &speechd.generation);
    if (err != ESP_OK) {
        return err;
    }

    if (!picotts_init(SPEECHD_TASK_PRIORITY, speechd_output, tskNO_AFFINITY)) {
        speechd.last_error = ESP_ERR_NO_MEM;
        return speechd.last_error;
    }
    speechd.engine_initialized = true;
    picotts_set_idle_notify(speechd_idle);
    picotts_set_error_notify(speechd_error);

    if (solar_os_task_create_pinned_internal(
            speechd_task,
            "speechd",
            SPEECHD_DISPATCH_STACK,
            NULL,
            SPEECHD_TASK_PRIORITY,
            &speechd.task,
            tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        picotts_shutdown();
        speechd.engine_initialized = false;
        speechd.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    while (!speechd.ready && !speechd.done) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    if (speechd.last_error != ESP_OK) {
        const esp_err_t start_error = speechd.last_error;
        speechd.stop_requested = true;
        solar_os_speech_worker_stop();
        (void)solar_os_task_wait_done(
            speechd.task, &speechd.done, SOLAR_OS_TASK_STOP_WAIT_MS);
        speechd.task = NULL;
        picotts_shutdown();
        speechd.engine_initialized = false;
        return start_error;
    }

    (void)solar_os_jobs_note_resource(solar_os_speechd_job.name,
                                      SOLAR_OS_JOB_RESOURCE_CUSTOM,
                                      "speech",
                                      speechd_voice());
    SOLAR_OS_LOGI(TAG, "started voice=%s", speechd_voice());
    return ESP_OK;
}

static void speechd_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    speechd.stop_requested = true;
    solar_os_speech_worker_stop();

    bool stopped = speechd.task == NULL || solar_os_task_wait_done(
        speechd.task, &speechd.done, SPEECHD_STOP_WAIT_MS);
    if (!stopped && speechd.engine_initialized) {
        SOLAR_OS_LOGW(TAG, "forcing PicoTTS shutdown after slow stop");
        speechd.engine_failed = true;
        picotts_shutdown();
        speechd.engine_initialized = false;
        if (speechd.engine_event != NULL) {
            (void)xSemaphoreGive(speechd.engine_event);
        }
        stopped = solar_os_task_wait_done(
            speechd.task, &speechd.done, SOLAR_OS_TASK_STOP_WAIT_MS);
    }
    if (!stopped) {
        SOLAR_OS_LOGW(TAG, "speech worker is slow to stop");
        while (!speechd.done) {
            vTaskDelay(pdMS_TO_TICKS(20U));
        }
    }
    speechd.task = NULL;

    if (speechd.engine_initialized) {
        picotts_set_idle_notify(NULL);
        picotts_set_error_notify(NULL);
        picotts_shutdown();
        speechd.engine_initialized = false;
    }
    speechd.player = NULL;
    speechd.current_id = 0U;
    SOLAR_OS_LOGI(TAG, "stopped");
}

static void speechd_detail(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
    solar_os_speech_queue_status_t status;
    solar_os_speech_queue_get_status(&status);
    solar_os_shell_io_printf(
        io,
        "  speech: voice=%s current=%" PRIu32 " state=%s queued=%u\n",
        speechd_voice(),
        status.current_id,
        status.current_id != 0U ?
            solar_os_speech_request_state_name(status.current_state) : "idle",
        (unsigned)status.queued);
    solar_os_shell_io_printf(
        io,
        "  requests: complete=%" PRIu32 " cancelled=%" PRIu32
        " dropped=%" PRIu32 " failed=%" PRIu32 "\n",
        status.completed,
        status.cancelled,
        status.dropped,
        status.failed);
}

static void speechd_error_detail(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0U) {
        return;
    }
    if (speechd.last_error == ESP_ERR_NO_MEM) {
        strlcpy(buffer,
                "PicoTTS needs about 1.1 MiB PSRAM plus worker stacks",
                buffer_len);
    } else {
        snprintf(buffer,
                 buffer_len,
                 "speech engine: %s",
                 esp_err_to_name(speechd.last_error));
    }
}

const solar_os_job_t solar_os_speechd_job = {
    .name = "speechd",
    .summary = "offline text-to-speech queue",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = speechd_start,
    .stop = speechd_stop,
    .worker_stack_bytes = SPEECHD_TOTAL_STACK,
    .worker_stack_external = false,
    .detail = speechd_detail,
    .error_detail = speechd_error_detail,
};
