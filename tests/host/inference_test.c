#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "solar_os_inference_backend.h"
#include "solar_os_model_bundle.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"

static atomic_int allocations, failures = -1, workers, backend_wait, backend_entered;
static bool deny_worker;
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t kind, const char *tag)
{
    (void)tag; assert(kind == SOLAR_OS_MEMORY_EXTERNAL_REQUIRED);
    if (atomic_load(&failures) >= 0 && atomic_fetch_sub(&failures, 1) == 0) return NULL;
    void *p = malloc(size); if (p) atomic_fetch_add(&allocations, 1); return p;
}
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t kind, const char *tag)
{
    void *p = solar_os_memory_alloc(n * size, kind, tag); if (p) memset(p, 0, n * size); return p;
}
void solar_os_memory_free(void *p)
{
    if (p) { assert(atomic_fetch_sub(&allocations, 1) > 0); free(p); }
}
int64_t esp_timer_get_time(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
void vTaskDelay(TickType_t ticks)
{
    struct timespec t = {.tv_sec = ticks / 1000, .tv_nsec = (ticks % 1000) * 1000000};
    nanosleep(&t, NULL);
}
void vTaskSuspend(TaskHandle_t task) { assert(!task); pthread_exit(NULL); }
struct test_task { pthread_t thread; TaskFunction_t run; void *user; };
static void *launch(void *user)
{
    struct test_task *t = user; t->run(t->user); return NULL;
}
BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t run, const char *name,
    uint32_t stack, void *user, UBaseType_t priority, TaskHandle_t *handle,
    BaseType_t core, solar_os_task_role_t role)
{
    (void)name; (void)priority; (void)core; (void)role;
    assert(stack == 12U * 1024U);
    if (deny_worker) return 0;
    struct test_task *t = malloc(sizeof(*t)); assert(t);
    t->run = run; t->user = user; *handle = t; atomic_fetch_add(&workers, 1);
    assert(!pthread_create(&t->thread, NULL, launch, t)); return pdPASS;
}
void solar_os_task_delete_internal(TaskHandle_t t)
{
    assert(!pthread_join(t->thread, NULL)); free(t); atomic_fetch_sub(&workers, 1);
}
/* Deliberately provide no solar_os_task_wait_done implementation: inference's
 * owner-deleted worker must not use the self-delete helper and its reap delay. */
struct solar_os_inference_backend { int resets; };
static esp_err_t tensor(solar_os_inference_tensor_t *t, const char *name)
{
    strcpy(t->name, name); t->dtype = SOLAR_OS_TENSOR_INT8;
    t->rank = 2; t->shape[0] = 2; t->shape[1] = 4; t->bytes = 8;
    t->exponent_count = 1;
    t->exponents = solar_os_memory_calloc(1, sizeof(int32_t), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test");
    return t->exponents ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t solar_os_inference_backend_load(const char *path, solar_os_inference_cancel_fn cancel,
    void *user, solar_os_inference_backend_t **out, solar_os_inference_model_info_t *info)
{
    if (strcmp(path, "/model.espdl")) return ESP_ERR_NOT_FOUND;
    if (cancel(user)) return ESP_ERR_TIMEOUT;
    *out = solar_os_memory_calloc(1, sizeof(**out), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test");
    if (!*out) return ESP_ERR_NO_MEM;
    info->input_count = info->output_count = 2;
    info->model_bytes = 1024;
    esp_err_t e = tensor(&info->inputs[0], "a"); if (e) return e;
    e = tensor(&info->inputs[1], "b"); if (e) return e;
    e = tensor(&info->outputs[0], "sum"); if (e) return e;
    return tensor(&info->outputs[1], "difference");
}
esp_err_t solar_os_inference_backend_run(solar_os_inference_backend_t *backend,
    const solar_os_inference_input_t *inputs, size_t count, solar_os_inference_cancel_fn cancel,
    void *user, solar_os_inference_result_t *r)
{
    (void)backend; assert(count == 2); atomic_store(&backend_entered, 1);
    while (atomic_load(&backend_wait)) { if (cancel(user)) return ESP_ERR_TIMEOUT; vTaskDelay(1); }
    if (cancel(user)) return ESP_ERR_TIMEOUT;
    const int8_t *a = inputs[strcmp(inputs[0].name, "a") ? 1 : 0].data;
    const int8_t *b = inputs[strcmp(inputs[0].name, "b") ? 1 : 0].data;
    for (size_t i = 0; i < 2; ++i) {
        esp_err_t e = tensor(&r->outputs[i].tensor, i ? "difference" : "sum"); if (e) return e;
        r->outputs[i].data = solar_os_memory_alloc(8, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test");
        if (!r->outputs[i].data) return ESP_ERR_NO_MEM;
        int8_t *data = r->outputs[i].data;
        for (size_t j = 0; j < 8; ++j) data[j] = i ? a[j] - b[j] : a[j] + b[j];
        ++r->count;
    }
    r->elapsed_us = 123; return ESP_OK;
}
void solar_os_inference_backend_reset(solar_os_inference_backend_t *b) { if (b) ++b->resets; }
void solar_os_inference_backend_set_mode(solar_os_inference_backend_t *b, solar_os_inference_mode_t mode)
{
    (void)b; assert((unsigned)mode <= SOLAR_OS_INFERENCE_DUAL);
}
void solar_os_inference_backend_close(solar_os_inference_backend_t *b) { solar_os_memory_free(b); }
#ifndef INFERENCE_NATIVE_BUNDLE_TEST
void solar_os_model_bundle_free(solar_os_model_bundle_t *bundle) { assert(!bundle); }
esp_err_t solar_os_model_bundle_load(const char *path, solar_os_inference_cancel_fn cancel,
    void *user, solar_os_model_bundle_t **bundle, solar_os_inference_backend_t **backend,
    solar_os_inference_model_info_t *info)
{ (void)path; (void)cancel; (void)user; (void)bundle; (void)backend; (void)info; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_model_bundle_run(const solar_os_model_bundle_t *bundle,
    const solar_os_inference_model_info_t *info, solar_os_inference_backend_t *backend,
    const solar_os_inference_value_t *values, size_t count, solar_os_inference_cancel_fn cancel,
    void *user, solar_os_inference_result_t *result)
{ (void)bundle; (void)info; (void)backend; (void)values; (void)count; (void)cancel; (void)user; (void)result; return ESP_ERR_NOT_SUPPORTED; }
#endif
static bool cancelled(void *user) { return atomic_load((atomic_int *)user); }
static const int8_t a[] = {1, -2, 3, 4, -5, 6, 7, -8}, b[] = {2, 3, -4, 1, 6, -2, 0, 4};
static solar_os_inference_input_t inputs[] = {{"b", b, sizeof(b), NULL}, {"a", a, sizeof(a), NULL}};
typedef struct { solar_os_inference_t *session; uint32_t id; esp_err_t status; } contender_t;
static void *compete(void *user)
{
    contender_t *c = user; solar_os_inference_result_t *r = NULL;
    c->status = solar_os_inference_run(c->session, c->id, inputs, 2, 1000, &r);
    solar_os_inference_result_free(r); return NULL;
}
int main(void)
{
    solar_os_inference_t *s = NULL, *other = NULL; uint32_t id;
    atomic_int cancellation = 0;
    assert(solar_os_inference_create(cancelled, &cancellation, &s) == ESP_OK);
    assert(solar_os_inference_create(NULL, NULL, &other) == ESP_OK);
    int baseline = allocations;
    for (int failure = 0; failure < 8; ++failure) {
        failures = failure;
        esp_err_t error = solar_os_inference_load(s, "/model.espdl", 1000, &id);
        failures = -1;
        if (!error) assert(solar_os_inference_close(s, id) == ESP_OK);
        assert(allocations == baseline && !workers);
    }
    deny_worker = true; assert(solar_os_inference_load(s, "/model.espdl", 1000, &id) == ESP_ERR_NO_MEM);
    deny_worker = false;
    assert(solar_os_inference_load(s, "/missing", 1000, &id) == ESP_ERR_NOT_FOUND);
    assert(solar_os_inference_load(s, "/model.espdl", 1000, &id) == ESP_OK);
    baseline = allocations;
    const solar_os_inference_model_info_t *info;
    assert(solar_os_inference_info(other, id, &info) == ESP_OK);
    assert(solar_os_inference_info(s, id, &info) == ESP_OK && info->input_count == 2);
    assert(info->mode == SOLAR_OS_INFERENCE_SINGLE);
    assert(solar_os_inference_set_mode(s, id, SOLAR_OS_INFERENCE_DUAL) == ESP_OK);
    assert(info->mode == SOLAR_OS_INFERENCE_SINGLE); /* Snapshot is immutable. */
    assert(solar_os_inference_info(s, id, &info) == ESP_OK);
    assert(info->mode == SOLAR_OS_INFERENCE_DUAL);
    assert(solar_os_inference_set_mode(s, id, 99) == ESP_ERR_INVALID_ARG);
    assert(solar_os_inference_set_mode(other, id, SOLAR_OS_INFERENCE_AUTO) == ESP_OK);
    solar_os_inference_mode_t mode;
    assert(solar_os_inference_mode_parse("auto", &mode) == ESP_OK && mode == SOLAR_OS_INFERENCE_AUTO);
    assert(solar_os_inference_mode_parse("bad", &mode) == ESP_ERR_INVALID_ARG);
    assert(solar_os_inference_set_mode(s, id, SOLAR_OS_INFERENCE_SINGLE) == ESP_OK);
    baseline = allocations;
    solar_os_inference_result_t *r = NULL;
    for (int failure = 0; failure < 9; ++failure) {
        failures = failure;
        esp_err_t e = solar_os_inference_run(s, id, inputs, 2, 1000, &r); failures = -1;
        if (!e) solar_os_inference_result_free(r);
        assert(allocations == baseline && !workers);
    }
    assert(solar_os_inference_run(s, id, inputs, 1, 1000, &r) == ESP_ERR_INVALID_ARG);
    inputs[0].name = "a"; assert(solar_os_inference_run(s, id, inputs, 2, 1000, &r) == ESP_ERR_INVALID_ARG);
    inputs[0].name = "unknown"; assert(solar_os_inference_run(s, id, inputs, 2, 1000, &r) == ESP_ERR_INVALID_ARG);
    inputs[0].name = "b"; inputs[0].bytes = 7;
    assert(solar_os_inference_run(s, id, inputs, 2, 1000, &r) == ESP_ERR_INVALID_SIZE); inputs[0].bytes = 8;
    solar_os_inference_tensor_t typed = info->inputs[1]; typed.dtype = SOLAR_OS_TENSOR_UINT8;
    inputs[0].tensor = &typed; assert(solar_os_inference_run(s, id, inputs, 2, 1000, &r) == ESP_ERR_INVALID_ARG);
    typed = info->inputs[1]; typed.shape[0] = 1;
    assert(solar_os_inference_run(s, id, inputs, 2, 1000, &r) == ESP_ERR_INVALID_ARG); inputs[0].tensor = NULL;
    assert(solar_os_inference_run(s, id, inputs, 2, 0, &r) == ESP_ERR_INVALID_ARG);
    cancellation = 1; assert(solar_os_inference_run(s, id, inputs, 2, 1000, &r) == ESP_ERR_TIMEOUT); cancellation = 0;
    atomic_store(&backend_wait, 1); atomic_store(&backend_entered, 0);
    contender_t c = {s, id, ESP_OK}; pthread_t thread; assert(!pthread_create(&thread, NULL, compete, &c));
    while (!atomic_load(&backend_entered)) vTaskDelay(1);
    uint32_t next; assert(solar_os_inference_load(other, "/model.espdl", 1000, &next) == ESP_ERR_INVALID_STATE);
    assert(solar_os_inference_close(s, id) == ESP_ERR_INVALID_STATE);
    assert(solar_os_inference_set_mode(s, id, SOLAR_OS_INFERENCE_DUAL) == ESP_ERR_INVALID_STATE);
    cancellation = 1; assert(!pthread_join(thread, NULL));
    assert(c.status == ESP_ERR_TIMEOUT && !workers && allocations == baseline);
    cancellation = 0;
    assert(solar_os_inference_run(s, id, inputs, 2, 5, &r) == ESP_ERR_TIMEOUT);
    atomic_store(&backend_wait, 0);
    /* A completed operation must succeed with a budget below the old 100 ms
     * reap delay. Existing cancellation tests also require worker join before
     * releasing backend/work/result allocations. */
    assert(solar_os_inference_run(s, id, inputs, 2, 50, &r) == ESP_OK);
    solar_os_inference_result_free(r);
    assert(allocations == baseline && !workers);
    for (int repeat = 0; repeat < 75; ++repeat) {
        assert(solar_os_inference_run(s, id, inputs, 2, 1000, &r) == ESP_OK);
        for (size_t j = 0; j < 8; ++j) {
            assert(((int8_t *)r->outputs[0].data)[j] == a[j] + b[j]);
            assert(((int8_t *)r->outputs[1].data)[j] == a[j] - b[j]);
        }
        solar_os_inference_result_free(r); assert(allocations == baseline && !workers);
    }
    assert(solar_os_inference_run(s, id, inputs, 2, 1000, &r) == ESP_OK);
    assert(solar_os_inference_close(s, id) == ESP_OK);
    assert(((int8_t *)r->outputs[0].data)[0] == 3); solar_os_inference_result_free(r);
    assert(solar_os_inference_run(s, id, inputs, 2, 1000, &r) == ESP_ERR_NOT_FOUND);
    assert(solar_os_inference_load(s, "/model.espdl", 1000, &next) == ESP_OK && next != id);
    assert(solar_os_inference_reset(s, next) == ESP_OK);
    assert(solar_os_inference_retain(next) == ESP_OK);
    assert(solar_os_inference_close(other, next) == ESP_ERR_INVALID_STATE);
    assert(solar_os_inference_close_all(other) == ESP_ERR_INVALID_STATE);
    assert(solar_os_inference_info(other, next, &info) == ESP_OK && info->references == 1);
    solar_os_inference_destroy(s); /* Loading client disappears, model remains. */
    uint32_t handles[4], found; size_t count;
    assert(solar_os_inference_list(handles, 4, &count) == ESP_OK && count == 1 && handles[0] == next);
    assert(solar_os_inference_find("/model.espdl", &found) == ESP_OK && found == next);
    assert(solar_os_inference_run(other, next, inputs, 2, 1000, &r) == ESP_OK);
    solar_os_inference_result_free(r);
    assert(solar_os_inference_release(next) == ESP_OK);
    assert(solar_os_inference_release(next) == ESP_ERR_INVALID_STATE);
    assert(solar_os_inference_close_all(other) == ESP_OK);
    assert(info->inputs[0].exponents[0] == 0); /* Snapshot survives unload. */
    assert(solar_os_inference_info(other, next, &info) == ESP_ERR_NOT_FOUND);
    assert(solar_os_inference_list(handles, 4, &count) == ESP_OK && count == 0);
    solar_os_inference_destroy(other);
    assert(!allocations && !workers);
    puts("inference ownership, named tensors, typed validation, busy, cancellation, OOM and repeated execution passed");
    return 0;
}
