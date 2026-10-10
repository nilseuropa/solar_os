#include "solar_os_pipeline.h"
#include "solar_os_processor.h"
#include "solar_os_script_media.h"
#include "solar_os_jobs.h"
#include "solar_os_task.h"
#include "solar_os_memory.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

#define PIPELINE_STACK 12288U
typedef struct {
    solar_os_job_t job;
    solar_os_pipeline_status_t status;
    solar_os_pipeline_config_t config;
    solar_os_processor_t *processor;
    TaskHandle_t worker;
    uint32_t generation;
    bool cancel, done;
    unsigned mailbox;
    char *latest;
} pipeline_t;
/* Cold state and buffers are in PSRAM. Only pointers/admission occupy SRAM. */
static pipeline_t **pipelines;
static unsigned admission;
static uint32_t next_id = 1;
static bool admit(void)
{ return !__atomic_exchange_n(&admission, 1, __ATOMIC_ACQUIRE); }
static void leave(void)
{ __atomic_store_n(&admission, 0, __ATOMIC_RELEASE); }
static void lock(pipeline_t *p)
{ while (__atomic_exchange_n(&p->mailbox, 1, __ATOMIC_ACQUIRE)) vTaskDelay(1); }
static void unlock(pipeline_t *p)
{ __atomic_store_n(&p->mailbox, 0, __ATOMIC_RELEASE); }
static bool cancelled(void *user)
{ return __atomic_load_n(&((pipeline_t *)user)->cancel, __ATOMIC_ACQUIRE); }
static pipeline_t *find(uint32_t id)
{
    if (pipelines) for (size_t i = 0; i < SOLAR_OS_PIPELINES_MAX; ++i)
        if (pipelines[i] && pipelines[i]->status.id == id) return pipelines[i];
    return NULL;
}
static void reap(pipeline_t *p)
{
    if (!p->worker) return;
    while (!__atomic_load_n(&p->done, __ATOMIC_ACQUIRE)) vTaskDelay(1);
    solar_os_task_delete_internal(p->worker); p->worker = NULL;
}
static void wait_interval(pipeline_t *p)
{
    uint32_t waited = 0;
    do {
        uint32_t step = p->config.interval_ms - waited;
        if (step > 20) step = 20;
        vTaskDelay(pdMS_TO_TICKS(step) ? pdMS_TO_TICKS(step) : 1);
        waited += step;
    } while (waited < p->config.interval_ms && !cancelled(p));
}
static void worker(void *user)
{
    pipeline_t *p = user;
    solar_os_script_media_t *media = NULL;
    uint32_t source = 0;
    const bool file = p->config.source[0] == '/';
    const bool rtsp = !strncmp(p->config.source, "rtsp://", 7);
    esp_err_t error = ESP_OK;
    if (!file) {
        error = solar_os_script_media_create(p->status.job, cancelled, p, &media);
        if (!error && rtsp) {
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
            error = solar_os_script_media_rtsp_open(media, p->config.source, true, false, &source);
#else
            error = ESP_ERR_NOT_SUPPORTED;
#endif
        } else if (!error) {
            solar_os_stream_open_options_t options = {
                .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE, .timeout_ms = 100,
                .requested_video = {.codec = SOLAR_OS_STREAM_VIDEO_JPEG},
            };
            error = solar_os_script_media_open(media, p->config.source, &options, &source);
        }
    }
    while (!error && !cancelled(p)) {
        solar_os_raster_image_t *image = NULL;
        uint32_t frame = 0;
        uint64_t timestamp = 0, begin = esp_timer_get_time(), decode_begin = begin;
        if (file) error = solar_os_raster_image_open(p->config.source, &image);
        else {
            if (rtsp) {
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
                error = solar_os_script_media_rtsp_read(media, source, 100, &frame);
                if (!error && !frame) {
                    solar_os_rtsp_client_status_t status; bool ended = false;
                    error = solar_os_script_media_rtsp_status(media, source, &status, &ended);
                    if (!error && ended) { error = status.error ? status.error : ESP_FAIL; break; }
                    continue;
                }
#endif
            } else error = solar_os_script_media_acquire(media, source, &frame);
            if (error == ESP_ERR_TIMEOUT) { error = ESP_OK; vTaskDelay(1); continue; }
            solar_os_script_media_frame_t info;
            if (!error) error = solar_os_script_media_frame(media, frame, &info);
            decode_begin = esp_timer_get_time();
            if (!error) {
                timestamp = info.jpeg.timestamp_us;
                error = solar_os_raster_image_decode(info.jpeg.data, info.jpeg.length, &image);
            }
            /* The compressed lease is released before recognition/inference. */
            if (frame) {
                esp_err_t released = solar_os_script_media_release(media, frame);
                if (!error) error = released;
            }
        }
        uint64_t decoded = esp_timer_get_time();
        char *json = NULL;
        bool processor_busy = false;
        if (!error && !cancelled(p)) {
            error = solar_os_processor_run(p->processor, image, &json);
            processor_busy = error == ESP_ERR_INVALID_STATE;
        }
        solar_os_raster_image_release(image);
        uint64_t end = esp_timer_get_time();
        if (processor_busy && !cancelled(p)) {
            lock(p); ++p->status.busy_frames; unlock(p);
            error = ESP_OK; wait_interval(p); continue;
        }
        if (!error && json && !cancelled(p)) {
            char *record = solar_os_memory_alloc(SOLAR_OS_PROCESSOR_JSON_MAX + 512,
                SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "pipeline.result");
            if (!record) error = ESP_ERR_NO_MEM;
            else {
                lock(p);
                uint64_t sequence = p->status.sequence + 1;
                int n = snprintf(record, SOLAR_OS_PROCESSOR_JSON_MAX + 512,
                    "{\"pipeline\":%lu,\"sequence\":%llu,\"source_timestamp_us\":%llu,"
                    "\"decode_us\":%llu,\"process_us\":%llu,\"frame_us\":%llu,\"result\":%s}",
                    (unsigned long)p->status.id, (unsigned long long)sequence,
                    (unsigned long long)timestamp, (unsigned long long)(decoded - decode_begin),
                    (unsigned long long)(end - decoded), (unsigned long long)(end - begin), json);
                if (n < 0 || (size_t)n >= SOLAR_OS_PROCESSOR_JSON_MAX + 512) error = ESP_ERR_INVALID_SIZE;
                else {
                    solar_os_memory_free(p->latest); p->latest = record; record = NULL;
                    p->status.sequence = sequence; ++p->status.frames;
                    p->status.source_timestamp_us = timestamp; p->status.decode_us = decoded - decode_begin;
                    p->status.process_us = end - decoded; p->status.frame_us = end - begin;
                }
                unlock(p); solar_os_memory_free(record);
            }
        }
        solar_os_memory_free(json);
        lock(p); bool finished = p->config.limit && p->status.frames >= p->config.limit; unlock(p);
        if (finished || (file && !p->config.limit)) break;
        if (!error) wait_interval(p);
    }
    if (media) {
        /* Keep ownership until providers acknowledge release. */
        esp_err_t closed;
        while ((closed = solar_os_script_media_close_all(media)) != ESP_OK) {
            lock(p); p->status.last_error = closed; unlock(p); vTaskDelay(pdMS_TO_TICKS(20));
        }
        solar_os_script_media_destroy(media);
    }
    solar_os_processor_destroy(p->processor); p->processor = NULL;
    lock(p); p->status.last_error = cancelled(p) ? ESP_OK : error; p->status.done = true; unlock(p);
    __atomic_store_n(&p->done, true, __ATOMIC_RELEASE);
    vTaskSuspend(NULL);
}
static esp_err_t start_job(void *user, solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argv;
    pipeline_t *p = user;
    if (argc) return ESP_ERR_INVALID_ARG;
    reap(p);
    __atomic_store_n(&p->cancel, false, __ATOMIC_RELEASE);
    __atomic_store_n(&p->done, false, __ATOMIC_RELEASE);
    lock(p); p->status.done = false; p->status.last_error = ESP_OK;
    p->status.frames = p->status.busy_frames = 0; unlock(p);
    esp_err_t error = solar_os_processor_create(p->config.processor, p->config.model,
        p->config.timeout_ms, cancelled, p, &p->processor);
    if (!error) error = solar_os_jobs_get_generation(p->status.job, &p->generation);
    if (!error && solar_os_task_create_pinned_internal(worker, p->status.job, PIPELINE_STACK,
        p, tskIDLE_PRIORITY + 1, &p->worker, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS)
        error = ESP_ERR_NO_MEM;
    if (error) {
        solar_os_processor_destroy(p->processor); p->processor = NULL;
        lock(p); p->status.done = true; p->status.last_error = error; unlock(p);
        __atomic_store_n(&p->done, true, __ATOMIC_RELEASE);
    } else {
        solar_os_jobs_note_resource(p->status.job, SOLAR_OS_JOB_RESOURCE_CUSTOM, p->config.processor, "native processor");
        solar_os_jobs_note_resource(p->status.job, p->config.source[0] == '/' ? SOLAR_OS_JOB_RESOURCE_FILE :
            !strncmp(p->config.source, "rtsp://", 7) ? SOLAR_OS_JOB_RESOURCE_NET : SOLAR_OS_JOB_RESOURCE_STREAM,
            p->config.source, "image source");
    }
    return error;
}
static void stop_job(void *user, solar_os_context_t *ctx)
{
    (void)ctx; pipeline_t *p = user;
    __atomic_store_n(&p->cancel, true, __ATOMIC_RELEASE); reap(p);
}
static bool event_job(void *user, solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx; (void)event; pipeline_t *p = user;
    if (__atomic_load_n(&p->done, __ATOMIC_ACQUIRE)) {
        reap(p);
        lock(p); esp_err_t error = p->status.last_error; unlock(p);
        solar_os_jobs_mark_stopped(p->status.job, p->generation, error);
    }
    return false;
}
esp_err_t solar_os_pipeline_start(const solar_os_pipeline_config_t *c, uint32_t *id)
{
    if (!id) return ESP_ERR_INVALID_ARG;
    *id = 0;
    if (!c || !c->source || !*c->source || strlen(c->source) >= 256 || !c->processor ||
        (strcmp(c->processor, "qr") && strcmp(c->processor, "model")) ||
        (!strcmp(c->processor, "qr") && c->model) || (!strcmp(c->processor, "model") && !c->model) ||
        !c->timeout_ms || c->timeout_ms > 60000 || c->interval_ms > 60000) return ESP_ERR_INVALID_ARG;
    if (!admit()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = ESP_ERR_NO_MEM;
    if (!pipelines) pipelines = solar_os_memory_calloc(SOLAR_OS_PIPELINES_MAX, sizeof(*pipelines),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "pipeline.registry");
    if (!pipelines) goto done;
    size_t slot = 0; while (slot < SOLAR_OS_PIPELINES_MAX && pipelines[slot]) ++slot;
    if (slot == SOLAR_OS_PIPELINES_MAX || next_id > 0x1fffffffU) goto done;
    pipeline_t *p = solar_os_memory_calloc(1, sizeof(*p), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "pipeline");
    if (!p) goto done;
    p->status.id = next_id++; p->status.model = c->model;
    strcpy(p->status.source, c->source); strcpy(p->status.processor, c->processor);
    snprintf(p->status.job, sizeof(p->status.job), "pipeline-%lu", (unsigned long)p->status.id);
    p->config = *c; p->config.source = p->status.source; p->config.processor = p->status.processor;
    p->job = (solar_os_job_t){.name = p->status.job, .summary = "Native image processor pipeline",
        .kind = SOLAR_OS_JOB_KIND_BACKGROUND, .callback_user = p, .start_with_user = start_job,
        .stop_with_user = stop_job, .event_with_user = event_job, .worker_stack_bytes = PIPELINE_STACK};
    error = solar_os_jobs_register_dynamic(p->status.job, p->job.summary, &p->job);
    if (error) { solar_os_memory_free(p); goto done; }
    pipelines[slot] = p;
    error = solar_os_jobs_start(NULL, p->status.job, 0, NULL);
    if (error) {
        /* Another client can start/stop the public job during admission.
         * Never free its descriptor unless the registry releases it. A record
         * retained after such contention remains discoverable through list. */
        esp_err_t stopped = solar_os_jobs_stop(NULL, p->status.job);
        if (!stopped && !solar_os_jobs_unregister_dynamic(p->status.job, &p->job)) {
            pipelines[slot] = NULL; solar_os_memory_free(p);
        }
    } else *id = p->status.id;
done:
    if (error && pipelines) {
        bool empty = true;
        for (size_t i = 0; i < SOLAR_OS_PIPELINES_MAX; ++i) if (pipelines[i]) empty = false;
        if (empty) { solar_os_memory_free(pipelines); pipelines = NULL; }
    }
    leave(); return error;
}
esp_err_t solar_os_pipeline_list(uint32_t *ids, size_t capacity, size_t *count)
{
    if (!count || (!ids && capacity)) return ESP_ERR_INVALID_ARG;
    if (!admit()) return ESP_ERR_INVALID_STATE;
    *count = 0;
    if (pipelines) for (size_t i = 0; i < SOLAR_OS_PIPELINES_MAX; ++i) if (pipelines[i]) {
        if (*count < capacity) ids[*count] = pipelines[i]->status.id;
        ++*count;
    }
    esp_err_t error = *count > capacity ? ESP_ERR_INVALID_SIZE : ESP_OK;
    leave(); return error;
}
esp_err_t solar_os_pipeline_status(uint32_t id, solar_os_pipeline_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    if (!admit()) return ESP_ERR_INVALID_STATE;
    pipeline_t *p = find(id); esp_err_t error = p ? ESP_OK : ESP_ERR_NOT_FOUND;
    if (p) {
        lock(p); *status = p->status; unlock(p);
        solar_os_job_status_t job;
        if (solar_os_jobs_get_by_name(p->status.job, &job)) {
            snprintf(status->state, sizeof(status->state), "%s", solar_os_job_state_name(job.state));
            if (job.state == SOLAR_OS_JOB_STOPPED || job.state == SOLAR_OS_JOB_FAILED) status->done = true;
            if (job.last_error && !status->last_error) status->last_error = job.last_error;
        }
    }
    leave(); return error;
}
esp_err_t solar_os_pipeline_result(uint32_t id, uint64_t after, char **json)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    *json = NULL; if (!admit()) return ESP_ERR_INVALID_STATE;
    pipeline_t *p = find(id); esp_err_t error = p ? ESP_OK : ESP_ERR_NOT_FOUND;
    if (p) {
        lock(p);
        if (p->latest && p->status.sequence > after) {
            size_t length = strlen(p->latest) + 1;
            *json = solar_os_memory_alloc(length, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "pipeline.snapshot");
            if (*json) memcpy(*json, p->latest, length); else error = ESP_ERR_NO_MEM;
        }
        unlock(p);
    }
    leave(); return error;
}
esp_err_t solar_os_pipeline_stop(uint32_t id)
{
    if (!admit()) return ESP_ERR_INVALID_STATE;
    pipeline_t *p = find(id);
    esp_err_t error = p ? solar_os_jobs_stop(NULL, p->status.job) : ESP_ERR_NOT_FOUND;
    leave(); return error;
}
esp_err_t solar_os_pipeline_destroy(uint32_t id)
{
    if (!admit()) return ESP_ERR_INVALID_STATE;
    pipeline_t *p = find(id);
    esp_err_t error = p ? solar_os_jobs_stop(NULL, p->status.job) : ESP_ERR_NOT_FOUND;
    if (p && !error) {
        /* A completing tick callback may still hold its descriptor reference. */
        while ((error = solar_os_jobs_unregister_dynamic(p->status.job, &p->job)) == ESP_ERR_INVALID_STATE) {
            solar_os_job_status_t job;
            if (!solar_os_jobs_get_by_name(p->status.job, &job) || job.state != SOLAR_OS_JOB_STOPPED) break;
            vTaskDelay(1);
        }
        if (!error) {
            for (size_t i = 0; i < SOLAR_OS_PIPELINES_MAX; ++i) if (pipelines[i] == p) pipelines[i] = NULL;
            solar_os_memory_free(p->latest); solar_os_memory_free(p);
            bool empty = true;
            for (size_t i = 0; i < SOLAR_OS_PIPELINES_MAX; ++i) if (pipelines[i]) empty = false;
            if (empty) { solar_os_memory_free(pipelines); pipelines = NULL; }
        }
    }
    leave(); return error;
}
