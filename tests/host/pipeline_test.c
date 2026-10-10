#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "solar_os_pipeline.h"
#include "solar_os_processor.h"
#include "solar_os_vision.h"
#include "solar_os_model_bundle.h"
#include "solar_os_script_media.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"
#include "solar_os_jobs.h"
#include "cJSON.h"

static atomic_int allocations, failures = -1, workers, references, leases, sources;
static atomic_int blocked, entered, busy, fail_decode, fail_acquire;
static bool deny_worker, bad_contract, raw_result;
static bool reject_stop, reject_unregister, stop_restarts;
static const solar_os_job_t *jobs[4];
static solar_os_job_status_t states[4];
static uint32_t generations[4];
void *solar_os_memory_alloc(size_t n, solar_os_memory_class_t kind, const char *tag)
{
    (void)kind; (void)tag;
    if (atomic_load(&failures) >= 0 && atomic_fetch_sub(&failures, 1) == 0) return NULL;
    void *p = malloc(n); if (p) ++allocations; return p;
}
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t kind, const char *tag)
{ void *p = solar_os_memory_alloc(n * size, kind, tag); if (p) memset(p, 0, n * size); return p; }
void solar_os_memory_free(void *p)
{ if (p) { assert(atomic_fetch_sub(&allocations, 1) > 0); free(p); } }
int64_t esp_timer_get_time(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
void vTaskDelay(TickType_t ticks)
{ struct timespec t = {.tv_sec = ticks / 1000, .tv_nsec = (ticks % 1000) * 1000000}; nanosleep(&t, NULL); }
void vTaskSuspend(TaskHandle_t task) { assert(!task); pthread_exit(NULL); }
struct test_task { pthread_t thread; TaskFunction_t fn; void *user; };
static void *thread(void *arg) { struct test_task *t = arg; t->fn(t->user); return NULL; }
BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t fn, const char *name, uint32_t size,
    void *user, UBaseType_t priority, TaskHandle_t *out, BaseType_t core, solar_os_task_role_t role)
{
    (void)name; (void)size; (void)priority; (void)core; (void)role;
    if (deny_worker) return 0;
    struct test_task *t = malloc(sizeof(*t)); assert(t); t->fn = fn; t->user = user;
    *out = t; ++workers; assert(!pthread_create(&t->thread, NULL, thread, t)); return pdPASS;
}
void solar_os_task_delete_internal(TaskHandle_t t)
{ assert(t); pthread_join(t->thread, NULL); free(t); --workers; }
static size_t job_index(const char *name)
{ for (size_t i = 0; i < 4; ++i) if (jobs[i] && !strcmp(jobs[i]->name, name)) return i; assert(0); return 0; }
esp_err_t solar_os_jobs_register_dynamic(const char *name, const char *summary, const solar_os_job_t *job)
{
    (void)name; (void)summary;
    for (size_t i = 0; i < 4; ++i) if (!jobs[i]) { jobs[i] = job; states[i] = (solar_os_job_status_t){0}; return ESP_OK; }
    return ESP_ERR_NO_MEM;
}
esp_err_t solar_os_jobs_unregister_dynamic(const char *name, const solar_os_job_t *job)
{ size_t i = job_index(name); if (reject_unregister) { reject_unregister = false; return ESP_ERR_INVALID_STATE; }
  assert(jobs[i] == job && states[i].state == SOLAR_OS_JOB_STOPPED); jobs[i] = NULL; return ESP_OK; }
esp_err_t solar_os_jobs_start(solar_os_context_t *ctx, const char *name, int argc, char **argv)
{
    size_t i = job_index(name); if (states[i].state == SOLAR_OS_JOB_RUNNING) jobs[i]->stop_with_user(jobs[i]->callback_user, ctx);
    ++generations[i]; esp_err_t e = jobs[i]->start_with_user(jobs[i]->callback_user, ctx, argc, argv);
    states[i].state = e ? SOLAR_OS_JOB_FAILED : SOLAR_OS_JOB_RUNNING; states[i].last_error = e; return e;
}
esp_err_t solar_os_jobs_stop(solar_os_context_t *ctx, const char *name)
{
    if (reject_stop) { reject_stop = false; return ESP_ERR_INVALID_STATE; }
    size_t i = job_index(name); if (states[i].state == SOLAR_OS_JOB_RUNNING) jobs[i]->stop_with_user(jobs[i]->callback_user, ctx);
    states[i].state = SOLAR_OS_JOB_STOPPED;
    if (stop_restarts) { stop_restarts = false; assert(!solar_os_jobs_start(ctx,name,0,NULL)); }
    return ESP_OK;
}
esp_err_t solar_os_jobs_get_generation(const char *name, uint32_t *out)
{ *out = generations[job_index(name)]; return ESP_OK; }
esp_err_t solar_os_jobs_mark_stopped(const char *name, uint32_t generation, esp_err_t e)
{ size_t i = job_index(name); assert(generation == generations[i]); states[i].state = SOLAR_OS_JOB_STOPPED; states[i].last_error = e; return ESP_OK; }
esp_err_t solar_os_jobs_note_resource(const char *n, solar_os_job_resource_type_t t, const char *r, const char *d)
{ (void)n; (void)t; (void)r; (void)d; return ESP_OK; }
bool solar_os_jobs_get_by_name(const char *name, solar_os_job_status_t *out)
{ *out = states[job_index(name)]; return true; }
const char *solar_os_job_state_name(solar_os_job_state_t state)
{ return state == SOLAR_OS_JOB_RUNNING ? "running" : state == SOLAR_OS_JOB_FAILED ? "failed" : "stopped"; }
static void *json_alloc(size_t n) { return solar_os_memory_alloc(n, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test.json"); }
esp_err_t solar_os_json_init(void)
{ cJSON_Hooks hooks = {json_alloc, solar_os_memory_free}; cJSON_InitHooks(&hooks); return ESP_OK; }
struct solar_os_raster_image { int unused; };
esp_err_t solar_os_raster_image_open(const char *path, solar_os_raster_image_t **out)
{
    if (!strcmp(path, "/missing.png")) return ESP_ERR_NOT_FOUND;
    *out = solar_os_memory_calloc(1, sizeof(**out), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test.image");
    return *out ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t solar_os_raster_image_decode(const uint8_t *data, size_t n, solar_os_raster_image_t **out)
{ assert(data && n && atomic_load(&leases) == 1); return fail_decode ? ESP_FAIL : solar_os_raster_image_open("/test.png", out); }
void solar_os_raster_image_release(solar_os_raster_image_t *image) { solar_os_memory_free(image); }
uint32_t solar_os_raster_image_width(const solar_os_raster_image_t *image) { (void)image; return 1280; }
uint32_t solar_os_raster_image_height(const solar_os_raster_image_t *image) { (void)image; return 960; }
esp_err_t solar_os_raster_image_pixels(const solar_os_raster_image_t *image, solar_os_raster_image_pixels_t *out)
{ (void)image; static const uint8_t rgb[3] = {1,2,3}; *out = (solar_os_raster_image_pixels_t){rgb,3,3,1,1}; return ESP_OK; }
esp_err_t solar_os_vision_qrcodes(solar_os_raster_image_t *image, const solar_os_raster_image_convert_options_t *o,
    solar_os_vision_cancel_fn cancel, void *user, solar_os_vision_qr_results_t **out)
{
    assert(o->output_width == 640 && o->output_height == 480);
    assert(image && !atomic_load(&leases)); ++entered;
    while (blocked && !cancel(user)) vTaskDelay(1);
    if (cancel(user)) return ESP_ERR_TIMEOUT;
    if (atomic_load(&busy) && atomic_fetch_sub(&busy, 1) > 0) return ESP_ERR_INVALID_STATE;
    *out = solar_os_memory_calloc(1, sizeof(**out), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test.qr");
    if (!*out) return ESP_ERR_NO_MEM;
    (*out)->count = 1; (*out)->width = 640; (*out)->height = 480;
    (*out)->codes[0].payload = solar_os_memory_alloc(3, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test.payload");
    if (!(*out)->codes[0].payload) { solar_os_memory_free(*out); *out = NULL; return ESP_ERR_NO_MEM; }
    memcpy((*out)->codes[0].payload, "\0\xff\x41", 3); (*out)->codes[0].length = 3;
    (*out)->codes[0].corners[0] = (solar_os_vision_point_t){31,42}; return ESP_OK;
}
void solar_os_vision_qr_results_free(solar_os_vision_qr_results_t *r)
{ if (r) { solar_os_memory_free(r->codes[0].payload); solar_os_memory_free(r); } }
struct solar_os_inference { solar_os_inference_cancel_fn cancel; void *user; solar_os_inference_model_info_t info; };
esp_err_t solar_os_inference_create(solar_os_inference_cancel_fn cancel, void *user, solar_os_inference_t **out)
{
    *out = solar_os_memory_calloc(1, sizeof(**out), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test.client");
    if (!*out) return ESP_ERR_NO_MEM;
    (*out)->cancel = cancel; (*out)->user = user;
    (*out)->info.bundle = true; (*out)->info.input_count = 1; (*out)->info.image_inputs[0] = !bad_contract;
    strcpy((*out)->info.inputs[0].name, "image"); return ESP_OK;
}
void solar_os_inference_destroy(solar_os_inference_t *c) { solar_os_memory_free(c); }
esp_err_t solar_os_inference_retain(uint32_t id) { if (id != 1) return ESP_ERR_NOT_FOUND; ++references; return ESP_OK; }
esp_err_t solar_os_inference_release(uint32_t id) { assert(id == 1 && atomic_load(&references)); --references; return ESP_OK; }
esp_err_t solar_os_inference_info(solar_os_inference_t *c, uint32_t id, const solar_os_inference_model_info_t **out)
{ assert(id == 1); *out = &c->info; return ESP_OK; }
const char *solar_os_tensor_dtype_name(solar_os_tensor_dtype_t dtype) { (void)dtype; return "int8"; }
esp_err_t solar_os_inference_run_bundle(solar_os_inference_t *c, uint32_t id, const solar_os_inference_value_t *v,
    size_t count, uint32_t timeout, solar_os_inference_result_t **out)
{
    (void)timeout; assert(id == 1 && count == 1 && v->image && !strcmp(v->tensor.name, "image"));
    assert(atomic_load(&references) && !atomic_load(&leases)); ++entered;
    while (blocked && !c->cancel(c->user)) vTaskDelay(1);
    if (c->cancel(c->user)) return ESP_ERR_TIMEOUT;
    *out = solar_os_memory_calloc(1, sizeof(**out), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test.result");
    if (!*out) return ESP_ERR_NO_MEM;
    const char *text = raw_result ? "{\"kind\":\"raw\"}" : "{\"kind\":\"classification\",\"classes\":[{\"label\":\"cat\"}]}";
    (*out)->result_json = solar_os_memory_alloc(strlen(text)+1, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test.json");
    (*out)->transforms_json = solar_os_memory_alloc(3, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test.transform");
    if (!(*out)->result_json || !(*out)->transforms_json) { solar_os_inference_result_free(*out); *out = NULL; return ESP_ERR_NO_MEM; }
    strcpy((*out)->result_json, text); strcpy((*out)->transforms_json, "{}");
    (*out)->inference_us = 1234;
    if (raw_result) { (*out)->count = 1; (*out)->outputs[0].tensor.bytes = 24001; }
    return ESP_OK;
}
void solar_os_inference_result_free(solar_os_inference_result_t *r)
{ if (r) { solar_os_memory_free(r->result_json); solar_os_memory_free(r->transforms_json); solar_os_memory_free(r); } }
struct solar_os_script_media { int unused; };
esp_err_t solar_os_script_media_create(const char *owner, solar_os_script_media_cancel_fn fn, void *user, solar_os_script_media_t **out)
{ (void)owner; (void)fn; (void)user; *out = solar_os_memory_calloc(1, sizeof(**out), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test.media"); return *out ? ESP_OK : ESP_ERR_NO_MEM; }
void solar_os_script_media_destroy(solar_os_script_media_t *s) { assert(!sources && !leases); solar_os_memory_free(s); }
esp_err_t solar_os_script_media_open(solar_os_script_media_t *s, const char *id, const solar_os_stream_open_options_t *options, uint32_t *out)
{ (void)s; (void)id; assert(options->requested_video.codec == SOLAR_OS_STREAM_VIDEO_JPEG); ++sources; *out = 1; return ESP_OK; }
esp_err_t solar_os_script_media_close_all(solar_os_script_media_t *s) { (void)s; assert(!leases); sources = 0; return ESP_OK; }
esp_err_t solar_os_script_media_acquire(solar_os_script_media_t *s, uint32_t source, uint32_t *out)
{ (void)s; assert(source && !leases); if (fail_acquire) return ESP_ERR_INVALID_STATE; ++leases; *out = 2; return ESP_OK; }
esp_err_t solar_os_script_media_frame(solar_os_script_media_t *s, uint32_t frame, solar_os_script_media_frame_t *out)
{ (void)s; assert(frame == 2 && leases == 1); static const uint8_t jpeg[] = {1,2,3}; *out = (solar_os_script_media_frame_t){.jpeg = {jpeg, 3, 1, 1, 999}}; return ESP_OK; }
esp_err_t solar_os_script_media_release(solar_os_script_media_t *s, uint32_t frame) { (void)s; assert(frame == 2 && leases == 1); --leases; return ESP_OK; }
esp_err_t solar_os_script_media_rtsp_open(solar_os_script_media_t *s, const char *url, bool video, bool audio, uint32_t *out)
{ assert(video && !audio); return solar_os_script_media_open(s, url, &(solar_os_stream_open_options_t){0}, out); }
esp_err_t solar_os_script_media_rtsp_read(solar_os_script_media_t *s, uint32_t id, uint32_t timeout, uint32_t *out)
{ (void)timeout; return solar_os_script_media_acquire(s, id, out); }
esp_err_t solar_os_script_media_rtsp_status(solar_os_script_media_t *s, uint32_t id, solar_os_rtsp_client_status_t *out, bool *ended)
{ (void)s; (void)id; memset(out, 0, sizeof(*out)); *ended = false; return ESP_OK; }
static solar_os_pipeline_status_t finish(uint32_t id)
{
    solar_os_pipeline_status_t s;
    for (int i = 0; i < 2000; ++i) {
        assert(!solar_os_pipeline_status(id, &s));
        if (s.done) { const solar_os_job_t *job = jobs[job_index(s.job)]; job->event_with_user(job->callback_user, NULL, NULL); return s; }
        vTaskDelay(1);
    }
    assert(0); return s;
}
static bool never_cancel(void *user) { (void)user; return false; }
int main(void)
{
    uint32_t id; char *json = NULL;
    solar_os_pipeline_config_t c = {.source = "/test.png", .processor = "qr", .timeout_ms = 10000};
    assert(!solar_os_pipeline_start(&c, &id));
    solar_os_pipeline_status_t s = finish(id); assert(s.frames == 1 && s.sequence == 1 && !s.last_error);
    assert(!solar_os_pipeline_result(id, 0, &json) && json);
    cJSON *r = cJSON_Parse(json); assert(r);
    cJSON *qr = cJSON_GetObjectItemCaseSensitive(r, "result"), *code = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(qr, "codes"), 0);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(code, "payload_hex")->valuestring, "00ff41"));
    assert(cJSON_GetArrayItem(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(code, "corners"), 0), 0)->valueint == 31);
    cJSON_Delete(r); solar_os_memory_free(json);
    assert(!solar_os_pipeline_result(id, 1, &json) && !json);
    assert(!solar_os_jobs_start(NULL, s.job, 0, NULL)); s = finish(id); assert(s.sequence == 2);
    assert(!solar_os_pipeline_destroy(id)); assert(!allocations && !workers);
    assert(solar_os_pipeline_status(id, &s) == ESP_ERR_NOT_FOUND);
    c.processor = "model"; c.model = 1; c.source = "rtsp://test/camera"; c.limit = 3;
    assert(!solar_os_pipeline_start(&c, &id)); s = finish(id);
    assert(s.frames == 3 && !s.last_error && s.source_timestamp_us == 999 && !references && !sources && !leases);
    assert(!solar_os_pipeline_result(id, 0, &json) && strstr(json, "classification") && strstr(json, "1234"));
    solar_os_memory_free(json); assert(!solar_os_pipeline_destroy(id)); assert(!allocations && !workers);
    c.source = "camera0"; c.limit = 0; blocked = 1; entered = 0;
    assert(!solar_os_pipeline_start(&c, &id)); while (!entered) vTaskDelay(1);
    assert(references == 1 && sources == 1 && !leases);
    int64_t start = esp_timer_get_time(); assert(!solar_os_pipeline_stop(id));
    assert(esp_timer_get_time() - start < 100000 && !references && !sources && !workers);
    assert(!solar_os_pipeline_destroy(id)); blocked = 0; assert(!allocations);
    /* A generic job restart can linearize after stop, before the API returns.
     * The controller must not reap that new continuous worker. */
    blocked = 1; entered = 0; assert(!solar_os_pipeline_start(&c, &id)); while (!entered) vTaskDelay(1);
    entered = 0; stop_restarts = true; assert(!solar_os_pipeline_stop(id)); while (!entered) vTaskDelay(1);
    assert(references == 1 && workers == 1); assert(!solar_os_pipeline_status(id,&s) && !strcmp(s.state,"running"));
    assert(!solar_os_pipeline_stop(id)); assert(!solar_os_pipeline_destroy(id)); blocked = 0;
    c.processor = "qr"; c.model = 0; c.source = "/test.png"; busy = 2;
    assert(!solar_os_pipeline_start(&c, &id)); s = finish(id); assert(s.busy_frames == 2 && s.frames == 1);
    assert(!solar_os_pipeline_destroy(id));
    c.source = "/missing.png"; assert(!solar_os_pipeline_start(&c, &id)); s = finish(id);
    assert(s.last_error == ESP_ERR_NOT_FOUND && !s.frames); assert(!solar_os_pipeline_destroy(id));
    c.source = "camera0"; fail_decode = 1; assert(!solar_os_pipeline_start(&c, &id)); s = finish(id);
    assert(s.last_error == ESP_FAIL && !sources && !leases); assert(!solar_os_pipeline_destroy(id)); fail_decode = 0;
    fail_acquire = 1; assert(!solar_os_pipeline_start(&c, &id)); s = finish(id);
    assert(s.last_error == ESP_ERR_INVALID_STATE && !s.busy_frames && !sources && !leases);
    assert(!solar_os_pipeline_destroy(id)); fail_acquire = 0;
    c.source = "/test.png";
    uint32_t ids[4]; size_t count;
    for (size_t i = 0; i < 4; ++i) { assert(!solar_os_pipeline_start(&c, &ids[i])); finish(ids[i]); }
    assert(solar_os_pipeline_start(&c, &id) == ESP_ERR_NO_MEM);
    assert(!solar_os_pipeline_list(ids, 4, &count) && count == 4);
    for (size_t i = 0; i < 4; ++i) assert(!solar_os_pipeline_destroy(ids[i]));
    assert(!allocations && !workers);
    /* Every SolarOS allocation failure must unwind native ownership. */
    for (int i = 0; i < 120; ++i) {
        failures = i; esp_err_t e = solar_os_pipeline_start(&c, &id);
        if (!e) { finish(id); assert(!solar_os_pipeline_destroy(id)); }
        failures = -1;
        assert(!allocations && !workers && !references && !leases && !sources);
    }
    c.processor = "model"; c.model = 1;
    for (int i = 0; i < 60; ++i) {
        failures = i; esp_err_t e = solar_os_pipeline_start(&c, &id);
        if (!e) { finish(id); assert(!solar_os_pipeline_destroy(id)); }
        failures = -1; assert(!allocations && !workers && !references);
    }
    c.processor = "qr"; c.model = 0;
    deny_worker = true; assert(solar_os_pipeline_start(&c, &id) == ESP_ERR_NO_MEM); deny_worker = false;
    assert(!solar_os_pipeline_start(&c, &id)); finish(id); assert(!solar_os_pipeline_destroy(id));
    solar_os_processor_t *p;
    bad_contract = true; assert(solar_os_processor_create("model", 1, 10000, never_cancel, NULL, &p) == ESP_ERR_NOT_SUPPORTED);
    c.processor = "model"; c.model = 1;
    reject_stop = true; assert(solar_os_pipeline_start(&c,&id) == ESP_ERR_NOT_SUPPORTED);
    assert(!solar_os_pipeline_list(ids,4,&count) && count == 1);
    assert(!solar_os_pipeline_status(ids[0],&s) && s.done); assert(!solar_os_pipeline_destroy(ids[0]));
    reject_unregister = true; assert(solar_os_pipeline_start(&c,&id) == ESP_ERR_NOT_SUPPORTED);
    assert(!solar_os_pipeline_list(ids,4,&count) && count == 1); assert(!solar_os_pipeline_destroy(ids[0]));
    bad_contract = false; assert(!references && !allocations);
    assert(!solar_os_processor_create("model", 1, 10000, never_cancel, NULL, &p));
    solar_os_raster_image_t *image; assert(!solar_os_raster_image_open("/test.png", &image)); raw_result = true;
    assert(solar_os_processor_run(p, image, &json) == ESP_ERR_INVALID_SIZE && !json);
    solar_os_raster_image_release(image); solar_os_processor_destroy(p); assert(!references && !allocations);
    puts("native pipeline ownership/cancellation/result tests passed");
    return 0;
}
