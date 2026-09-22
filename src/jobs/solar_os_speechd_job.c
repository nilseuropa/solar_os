#include "solar_os_speechd_job.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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
#include "solar_os_memory.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"
#include "solar_os_speech.h"
#include "solar_os_storage.h"
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
#define SPEECHD_VOICE_NAME_MAX 24U
#define SPEECHD_ERROR_DETAIL_MAX 192U
#define SPEECHD_RESOURCE_MAX_BYTES (2U * 1024U * 1024U)
#define SPEECHD_TA_FILENAME "ta.bin"
#define SPEECHD_SG_FILENAME "sg.bin"

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
    void *ta_data;
    size_t ta_size;
    void *sg_data;
    size_t sg_size;
    char voice_dir[SOLAR_OS_STORAGE_PATH_MAX];
    char voice[SPEECHD_VOICE_NAME_MAX];
    char last_error_detail[SPEECHD_ERROR_DETAIL_MAX];
    uint32_t current_id;
    uint32_t generation;
    esp_err_t last_error;
} speechd_state_t;

static EXT_RAM_BSS_ATTR speechd_state_t speechd;
static const char *TAG = "solar_os_speechd";

static void speechd_set_error(esp_err_t error, const char *detail)
{
    speechd.last_error = error;
    strlcpy(speechd.last_error_detail,
            detail != NULL ? detail : "",
            sizeof(speechd.last_error_detail));
}

static void speechd_release_voice_resources(void)
{
    solar_os_memory_free(speechd.ta_data);
    solar_os_memory_free(speechd.sg_data);
    speechd.ta_data = NULL;
    speechd.sg_data = NULL;
    speechd.ta_size = 0U;
    speechd.sg_size = 0U;
}

static bool speechd_cleanup_stopped(void)
{
    speechd.task = NULL;
    if (speechd.engine_initialized) {
        picotts_set_progress_notify(NULL);
        picotts_set_idle_notify(NULL);
        picotts_set_error_notify(NULL);
        if (!picotts_shutdown()) {
            return false;
        }
        speechd.engine_initialized = false;
    }
    speechd_release_voice_resources();
    speechd.player = NULL;
    speechd.current_id = 0U;
    return true;
}

static void speechd_reap_stopped(void)
{
    if (speechd.task != NULL && speechd.done) {
        if (!solar_os_task_wait_done(
                speechd.task, &speechd.done, SOLAR_OS_TASK_STOP_WAIT_MS)) {
            return;
        }
    }
    if (speechd.task == NULL || speechd.done) {
        (void)speechd_cleanup_stopped();
    }
}

static esp_err_t speechd_load_resource(const char *path,
                                       const char *label,
                                       void **data,
                                       size_t *size)
{
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
        char detail[SPEECHD_ERROR_DETAIL_MAX];
        snprintf(detail, sizeof(detail), "%s resource not found: %s", label, path);
        speechd_set_error(ESP_ERR_NOT_FOUND, detail);
        return ESP_ERR_NOT_FOUND;
    }
    if (info.st_size <= 0 || (uint64_t)info.st_size > SPEECHD_RESOURCE_MAX_BYTES) {
        char detail[SPEECHD_ERROR_DETAIL_MAX];
        snprintf(detail, sizeof(detail), "%s resource has invalid size: %s", label, path);
        speechd_set_error(ESP_ERR_INVALID_SIZE, detail);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        char detail[SPEECHD_ERROR_DETAIL_MAX];
        snprintf(detail, sizeof(detail), "cannot open %s resource: %s", label, path);
        speechd_set_error(ESP_FAIL, detail);
        return ESP_FAIL;
    }

    const size_t bytes = (size_t)info.st_size;
    void *buffer = solar_os_memory_alloc(
        bytes, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, label);
    if (buffer == NULL) {
        fclose(file);
        char detail[SPEECHD_ERROR_DETAIL_MAX];
        snprintf(detail,
                 sizeof(detail),
                 "not enough PSRAM for %s resource (%u bytes)",
                 label,
                 (unsigned)bytes);
        speechd_set_error(ESP_ERR_NO_MEM, detail);
        return ESP_ERR_NO_MEM;
    }

    const size_t read_bytes = fread(buffer, 1U, bytes, file);
    const bool read_failed = read_bytes != bytes || ferror(file);
    fclose(file);
    if (read_failed) {
        solar_os_memory_free(buffer);
        char detail[SPEECHD_ERROR_DETAIL_MAX];
        snprintf(detail, sizeof(detail), "cannot read %s resource: %s", label, path);
        speechd_set_error(ESP_FAIL, detail);
        return ESP_FAIL;
    }

    *data = buffer;
    *size = bytes;
    return ESP_OK;
}

static void speechd_set_voice_name(const char *directory)
{
    const char *end = directory + strlen(directory);
    while (end > directory && end[-1] == '/') {
        end--;
    }
    const char *start = end;
    while (start > directory && start[-1] != '/') {
        start--;
    }
    size_t length = (size_t)(end - start);
    if (length == 0U) {
        start = "unknown";
        length = strlen(start);
    }
    if (length >= sizeof(speechd.voice)) {
        length = sizeof(speechd.voice) - 1U;
    }
    memcpy(speechd.voice, start, length);
    speechd.voice[length] = '\0';
}

static esp_err_t speechd_load_voice(solar_os_context_t *ctx,
                                    const char *directory)
{
    esp_err_t err = solar_os_shell_resolve_path(
        ctx, directory, speechd.voice_dir, sizeof(speechd.voice_dir));
    if (err != ESP_OK) {
        speechd_set_error(err, "invalid or oversized voice directory path");
        return err;
    }

    struct stat info;
    if (stat(speechd.voice_dir, &info) != 0 || !S_ISDIR(info.st_mode)) {
        char detail[SPEECHD_ERROR_DETAIL_MAX];
        snprintf(detail,
                 sizeof(detail),
                 "voice directory not found: %s",
                 speechd.voice_dir);
        speechd_set_error(ESP_ERR_NOT_FOUND, detail);
        return ESP_ERR_NOT_FOUND;
    }

    char ta_path[SOLAR_OS_STORAGE_PATH_MAX];
    char sg_path[SOLAR_OS_STORAGE_PATH_MAX];
    err = solar_os_storage_join_path(speechd.voice_dir,
                                     SPEECHD_TA_FILENAME,
                                     ta_path,
                                     sizeof(ta_path));
    if (err == ESP_OK) {
        err = solar_os_storage_join_path(speechd.voice_dir,
                                         SPEECHD_SG_FILENAME,
                                         sg_path,
                                         sizeof(sg_path));
    }
    if (err != ESP_OK) {
        speechd_set_error(err, "voice resource path is too long");
        return err;
    }

    err = speechd_load_resource(
        ta_path, "picotts-ta", &speechd.ta_data, &speechd.ta_size);
    if (err == ESP_OK) {
        err = speechd_load_resource(
            sg_path, "picotts-sg", &speechd.sg_data, &speechd.sg_size);
    }
    if (err != ESP_OK) {
        speechd_release_voice_resources();
        return err;
    }
    speechd_set_voice_name(speechd.voice_dir);
    return ESP_OK;
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

static void speechd_progress(unsigned bytes_done, unsigned bytes_total)
{
    if (speechd.current_id != 0U) {
        (void)solar_os_speech_worker_set_progress(
            speechd.current_id, bytes_done, bytes_total);
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

static void speechd_wait_engine_idle(void)
{
    while (!speechd.engine_idle && !speechd.engine_failed &&
           !speechd.stop_requested) {
        (void)xSemaphoreTake(speechd.engine_event, pdMS_TO_TICKS(50U));
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
    const volatile bool *cancelled =
        solar_os_speech_worker_cancel_flag(work->id);
    speechd_drain_engine_event();
    speechd.engine_idle = false;
    speechd.awaiting_idle = true;
    const bool queued = picotts_add(
        work->text,
        strlen(work->text) + 1U,
        work->pitch,
        work->speed,
        cancelled);

    if (!queued && !speechd.stop_requested &&
        (cancelled == NULL || !*cancelled)) {
        speechd.output_error = ESP_FAIL;
        speechd.engine_failed = true;
    }
    if (!queued) {
        speechd.awaiting_idle = false;
    }

    if (queued) {
        speechd_wait_engine_idle();
    }

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

static void speechd_run_stream(const solar_os_speech_work_t *work)
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
    const volatile bool *cancelled =
        solar_os_speech_worker_cancel_flag(work->id);
    speechd_drain_engine_event();
    speechd.engine_idle = false;
    speechd.awaiting_idle = true;
    bool stream_open = picotts_stream_begin(
        work->pitch, work->speed, cancelled);
    bool final_queued = false;

    while (stream_open && !final_queued && !speechd.engine_failed &&
           !speechd.stop_requested &&
           (cancelled == NULL || !*cancelled)) {
        char text[SOLAR_OS_SPEECH_TEXT_MAX + 1U];
        size_t text_len = 0U;
        bool final = false;
        err = solar_os_speech_worker_stream_take(
            work->id,
            text,
            sizeof(text),
            &text_len,
            &final,
            UINT32_MAX);
        if (err != ESP_OK) {
            if (err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
                speechd.output_error = err;
            }
            break;
        }
        const bool accepted = text_len > 0U ?
            picotts_stream_write(text,
                                 (unsigned)text_len,
                                 final,
                                 cancelled) :
            picotts_stream_end(cancelled);
        if (!accepted) {
            if (!speechd.stop_requested &&
                (cancelled == NULL || !*cancelled)) {
                speechd.output_error = ESP_FAIL;
                speechd.engine_failed = true;
            }
            break;
        }
        final_queued = final;
    }

    if (stream_open && !final_queued && !speechd.engine_failed &&
        !speechd.stop_requested) {
        /* Close Pico's open utterance after cancellation so the persistent
         * engine is clean for the next request. Audio output is suppressed by
         * the request's cancellation flag while it drains. */
        final_queued = picotts_stream_end(NULL);
        if (!final_queued) {
            speechd.output_error = ESP_FAIL;
            speechd.engine_failed = true;
        }
    }
    if (final_queued) {
        speechd_wait_engine_idle();
    } else {
        speechd.awaiting_idle = false;
    }

    speechd_close_player(cancelled);

    solar_os_speech_request_state_t state = SOLAR_OS_SPEECH_REQUEST_COMPLETE;
    err = speechd.output_error;
    if (cancelled != NULL && *cancelled) {
        state = SOLAR_OS_SPEECH_REQUEST_CANCELLED;
        err = ESP_ERR_TIMEOUT;
    } else if (speechd.engine_failed || err != ESP_OK || !final_queued) {
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
                if (work.streaming) {
                    speechd_run_stream(&work);
                } else {
                    speechd_run_request(&work);
                }
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
    if (argc != 2 || argv == NULL || argv[0] == NULL || argv[1] == NULL ||
        strcmp(argv[0], solar_os_speechd_job.name) != 0 || argv[1][0] == '\0') {
        speechd_set_error(
            ESP_ERR_INVALID_ARG, "usage: job start speechd <voice-directory>");
        return ESP_ERR_INVALID_ARG;
    }
    speechd_set_error(ESP_OK, "");
    if (!solar_os_audio_output_available()) {
        speechd_set_error(ESP_ERR_NOT_SUPPORTED, "audio output is unavailable");
        return ESP_ERR_NOT_SUPPORTED;
    }
    speechd_reap_stopped();
    if (speechd.task != NULL || speechd.engine_initialized) {
        speechd_set_error(ESP_ERR_INVALID_STATE, "speechd is already running");
        return ESP_ERR_INVALID_STATE;
    }
    if (speechd.engine_event == NULL) {
        speechd.engine_event = xSemaphoreCreateBinaryStatic(
            &speechd.engine_event_storage);
    }
    if (speechd.engine_event == NULL) {
        speechd_set_error(
            ESP_ERR_NO_MEM, "cannot allocate the speechd engine event");
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
    speechd.voice_dir[0] = '\0';
    speechd.voice[0] = '\0';
    speechd.current_id = 0U;
    esp_err_t err = solar_os_jobs_get_generation(
        solar_os_speechd_job.name, &speechd.generation);
    if (err != ESP_OK) {
        speechd_set_error(err, "cannot determine speechd job generation");
        return err;
    }

    err = speechd_load_voice(ctx, argv[1]);
    if (err != ESP_OK) {
        return err;
    }
    if (!picotts_init_resources(SPEECHD_TASK_PRIORITY,
                                speechd_output,
                                tskNO_AFFINITY,
                                speechd.ta_data,
                                speechd.ta_size,
                                speechd.sg_data,
                                speechd.sg_size)) {
        speechd_set_error(
            ESP_FAIL, "PicoTTS rejected the voice blobs or lacked PSRAM");
        speechd_release_voice_resources();
        return speechd.last_error;
    }
    speechd.engine_initialized = true;
    picotts_set_progress_notify(speechd_progress);
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
        speechd_set_error(
            ESP_ERR_NO_MEM, "cannot allocate the speechd dispatcher task");
        speechd.task = NULL;
        (void)speechd_cleanup_stopped();
        return ESP_ERR_NO_MEM;
    }
    while (!speechd.ready && !speechd.done) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    if (speechd.last_error != ESP_OK) {
        const esp_err_t start_error = speechd.last_error;
        speechd.stop_requested = true;
        solar_os_speech_worker_stop();
        if (!solar_os_task_wait_done(
                speechd.task, &speechd.done, SOLAR_OS_TASK_STOP_WAIT_MS)) {
            speechd_set_error(
                ESP_ERR_TIMEOUT,
                "speech worker is still stopping; resources retained safely");
            return start_error;
        }
        (void)speechd_cleanup_stopped();
        return start_error;
    }

    (void)solar_os_jobs_note_resource(solar_os_speechd_job.name,
                                      SOLAR_OS_JOB_RESOURCE_CUSTOM,
                                      "speech",
                                      speechd.voice_dir);
    SOLAR_OS_LOGI(TAG,
                  "started voice=%s directory=%s resources=%u",
                  speechd.voice,
                  speechd.voice_dir,
                  (unsigned)(speechd.ta_size + speechd.sg_size));
    return ESP_OK;
}

static void speechd_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    speechd.stop_requested = true;
    solar_os_speech_worker_stop();

    const bool stopped = speechd.task == NULL || solar_os_task_wait_done(
        speechd.task, &speechd.done, SPEECHD_STOP_WAIT_MS);
    if (!stopped) {
        speechd_set_error(
            ESP_ERR_TIMEOUT,
            "speech worker is still stopping; resources retained safely");
        SOLAR_OS_LOGE(TAG, "%s", speechd.last_error_detail);
        return;
    }
    if (!speechd_cleanup_stopped()) {
        speechd_set_error(
            ESP_ERR_TIMEOUT,
            "PicoTTS is still stopping; resources retained safely");
        SOLAR_OS_LOGE(TAG, "%s", speechd.last_error_detail);
        return;
    }
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
        "  speech: engine=picotts voice=%s current=%" PRIu32
        " state=%s queued=%u\n",
        speechd.voice[0] != '\0' ? speechd.voice : "none",
        status.current_id,
        status.current_id != 0U ?
            solar_os_speech_request_state_name(status.current_state) : "idle",
        (unsigned)status.queued);
    solar_os_shell_io_printf(
        io,
        "  resources: directory=%s bytes=%u\n",
        speechd.voice_dir[0] != '\0' ? speechd.voice_dir : "none",
        (unsigned)(speechd.ta_size + speechd.sg_size));
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
    if (speechd.last_error_detail[0] != '\0') {
        strlcpy(buffer, speechd.last_error_detail, buffer_len);
    } else if (speechd.last_error == ESP_ERR_NO_MEM) {
        strlcpy(buffer,
                "PicoTTS needs 1.1 MiB PSRAM plus the selected voice blobs",
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
