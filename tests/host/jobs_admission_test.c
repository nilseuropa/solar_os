/* Actual registry implementation; deterministic interleavings at unlocked
 * preflight boundaries expose descriptor/pending lifetime errors under ASan. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_jobs.h"
#include "solar_os_native_jobs.h"
#include "solar_os_native_core.h"
#include "solar_os_task.h"
#include "solar_os_memory.h"
#include "solar_os_log.h"
#include "jobs/solar_os_job_registry.h"

static unsigned depth, exit_count, hook_exit;
static void (*exit_hook)(void);
static bool can_create = true;
static int allocations, starts, stops;
static solar_os_job_t *descriptor;
static solar_os_context_t context;
static bool callback_active;
static bool expire_claim;
static solar_os_native_handle_t claimed_port;
int solar_os_native_port_claim(const char *name, const char *owner, solar_os_native_handle_t *out)
{
    assert(!depth && !claimed_port && !strcmp(name, "uart0") && !strcmp(owner, "job:dynamic"));
    *out = claimed_port = 42;
    if (expire_claim) assert(solar_os_jobs_stop(NULL, "dynamic") == ESP_OK);
    return SOLAR_OS_NATIVE_OK;
}
int solar_os_native_port_close(solar_os_native_handle_t handle)
{
    assert(handle && handle == claimed_port);
    claimed_port = 0;
    return SOLAR_OS_NATIVE_OK;
}
solar_os_context_t *solar_os_application_context(void) { return callback_active ? &context : NULL; }
void test_enter_critical(void) { ++depth; }
void test_leave_critical(void)
{
    assert(depth); --depth;
    if (!depth && ++exit_count == hook_exit && exit_hook) {
        void (*hook)(void) = exit_hook; exit_hook = NULL; hook();
    }
}
void vTaskDelay(TickType_t ticks) { (void)ticks; assert(0); }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
size_t strlcpy(char *out, const char *source, size_t size)
{ size_t n = strlen(source); if (size) { size_t copy = n < size ? n : size-1; memcpy(out,source,copy); out[copy]=0; } return n; }
void *solar_os_memory_alloc(size_t n, solar_os_memory_class_t kind, const char *tag)
{ (void)kind; (void)tag; assert(!depth); void *p = malloc(n); assert(p); ++allocations; return p; }
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t kind, const char *tag)
{ void *p = solar_os_memory_alloc(n*size,kind,tag); memset(p,0,n*size); return p; }
void solar_os_memory_free(void *p) { assert(!depth); if (p) { --allocations; free(p); } }
bool solar_os_task_can_create(uint32_t stack, solar_os_task_role_t role, bool external)
{ assert(!depth && stack == 4096 && role == SOLAR_OS_TASK_ROLE_BACKGROUND && !external); return can_create; }
void solar_os_task_note_wait_queued(void) {}
void solar_os_task_note_wait_finished(bool launched) { (void)launched; }
size_t solar_os_job_registry_count(void) { return 0; }
const solar_os_job_registry_entry_t *solar_os_job_registry_get(size_t i) { (void)i; return NULL; }
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{ (void)level; (void)tag; (void)fmt; return ESP_OK; }
int64_t esp_timer_get_time(void) { return 0; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test error"; }
static esp_err_t start(solar_os_context_t *ctx, int argc, char **argv)
{ (void)ctx; (void)argv; assert(!depth && argc == 0); ++starts; return ESP_OK; }
static void stop(solar_os_context_t *ctx) { (void)ctx; assert(!depth); ++stops; }
static void detail(solar_os_context_t *ctx)
{
    assert(ctx == &context && !depth);
    assert(solar_os_jobs_unregister_dynamic("dynamic", descriptor) == ESP_ERR_INVALID_STATE);
}
static void error_detail(char *buffer, size_t capacity)
{
    detail(&context);
    strlcpy(buffer, "test detail", capacity);
}
static void register_job(void)
{
    descriptor = calloc(1, sizeof(*descriptor) + 32); assert(descriptor);
    char *text = (char *)(descriptor + 1);
    strcpy(text, "dynamic");
    strcpy(text + 8, "admission test");
    *descriptor = (solar_os_job_t){.name=text,.summary=text+8,
        .kind=SOLAR_OS_JOB_KIND_BACKGROUND,.start=start,.stop=stop,.worker_stack_bytes=4096,
        .detail=detail,.error_detail=error_detail};
    assert(!solar_os_jobs_register_dynamic(descriptor->name,descriptor->summary,descriptor));
}
static void release_job(void)
{
    assert(!solar_os_jobs_stop(NULL,"dynamic"));
    assert(!solar_os_jobs_unregister_dynamic("dynamic",descriptor));
    free(descriptor); descriptor = NULL;
}
#include "../../src/solar_os_jobs.c"
int main(void)
{
    const solar_os_native_jobs_api_v1_t *api =
        solar_os_native_jobs_get_service("jobs", 1, sizeof(*api));
    assert(api && !solar_os_native_jobs_get_service("jobs", 2, 8));
    assert(!solar_os_native_jobs_get_service("jobs", 1, sizeof(*api) + 1));
    register_job();
    solar_os_job_inspection_v1_t snapshot;
    assert(api->find_owner("job:dynamic", &snapshot) == SOLAR_OS_NATIVE_OK && snapshot.status.has_detail);
    assert(snapshot.disposition == SOLAR_OS_JOB_START_READY && api->count() == 1);
    char error[20];
    assert(api->error_detail("dynamic", error, sizeof(error)) && !strcmp(error, "test detail"));
    assert(api->print_detail("dynamic") == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    callback_active = true;
    assert(api->print_detail("dynamic") == SOLAR_OS_NATIVE_OK);
    callback_active = false;
    assert(api->start("dynamic", 0, NULL) == SOLAR_OS_NATIVE_OK);
    assert(starts == 1); release_job(); assert(stops == 1 && !allocations);
    assert(!strcmp(snapshot.status.name, "dynamic") && !strcmp(snapshot.status.summary, "admission test"));
    /* Admission was copied, but the descriptor disappears before preflight.
     * A queued request allocated afterwards must also be released on lookup. */
    register_job(); can_create = false;
    exit_count = 0; hook_exit = 2; exit_hook = release_job;
    assert(solar_os_jobs_start(NULL,"dynamic",0,NULL) == ESP_ERR_NOT_FOUND);
    assert(!descriptor && !allocations && !exit_hook);
    /* A stopped/unregistered queued job can vanish between retry's registry
     * snapshot and memory preflight. Neither its descriptor nor request lives. */
    register_job(); assert(!solar_os_jobs_start(NULL,"dynamic",0,NULL)); assert(allocations == 1);
    can_create = true; exit_count = 0; hook_exit = 1; exit_hook = release_job;
    job_retry_pending_start(0,NULL);
    assert(!descriptor && !allocations && !exit_hook && starts == 1);
    /* Ordinary queued admission still runs exactly once. */
    register_job(); can_create = false; assert(!solar_os_jobs_start(NULL,"dynamic",0,NULL));
    can_create = true; job_retry_pending_start(0,NULL); assert(starts == 2 && !allocations);
    release_job(); assert(stops == 2 && !depth && !allocations);
    register_job();
    assert(api->start("dynamic", 0, NULL) == SOLAR_OS_NATIVE_OK);
    assert(!api->find("dynamic", &snapshot));
    const uint32_t old_generation = snapshot.status.generation;
    assert(!api->owner_name("dynamic", error, sizeof(error)) && !strcmp(error, "job:dynamic"));
    assert(!api->note_resource("dynamic", old_generation, SOLAR_OS_JOB_RESOURCE_FILE, "cache", "read"));
    assert(!api->find("dynamic", &snapshot) && snapshot.status.resource_count == 1);
    assert(!api->clear_resources("dynamic", old_generation));
    solar_os_native_handle_t port;
    expire_claim = true;
    assert(api->claim_port("dynamic", old_generation, "uart0", &port) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!port && !claimed_port && stops == 3);
    expire_claim = false;
    release_job();

    register_job();
    assert(!api->start("dynamic", 0, NULL) && !api->find("dynamic", &snapshot));
    const uint32_t generation = snapshot.status.generation;
    assert(generation != old_generation);
    assert(api->note_resource("dynamic", old_generation, SOLAR_OS_JOB_RESOURCE_FILE, "cache", NULL) ==
        SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!api->claim_port("dynamic", generation, "uart0", &port) && port == claimed_port);
    assert(api->clear_resources("dynamic", old_generation) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!api->find("dynamic", &snapshot) && snapshot.status.resource_count == 1);
    assert(!strcmp(snapshot.status.resources[0].name, "uart0"));
    assert(!solar_os_native_port_close(port));
    assert(!api->complete("dynamic", generation, SOLAR_OS_NATIVE_ERROR_TIMEOUT));
    assert(!api->find("dynamic", &snapshot) && snapshot.status.state == SOLAR_OS_JOB_STOPPED &&
        snapshot.status.last_error == SOLAR_OS_NATIVE_ERROR_TIMEOUT && !snapshot.status.resource_count);
    assert(api->complete("dynamic", generation, SOLAR_OS_NATIVE_OK) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    release_job();

    register_job();
    assert(solar_os_jobs_start(NULL,"dynamic",0,NULL) == ESP_OK);
    assert(solar_os_shutdown_request() == ESP_OK);
    assert(solar_os_jobs_start(NULL,"dynamic",0,NULL) == ESP_ERR_INVALID_STATE);
    job_runtimes[0].state = SOLAR_OS_JOB_STOPPED;
    job_runtimes[0].lifecycle_busy = true;
    assert(solar_os_jobs_prepare_shutdown(NULL) == ESP_ERR_NOT_FINISHED);
    job_runtimes[0].lifecycle_busy = false;
    job_runtimes[0].state = SOLAR_OS_JOB_RUNNING;
    job_runtimes[0].callback_refs = 1;
    assert(solar_os_jobs_prepare_shutdown(NULL) == ESP_ERR_NOT_FINISHED);
    assert(stops == 3 && job_runtimes[0].state == SOLAR_OS_JOB_RUNNING);
    job_runtimes[0].callback_refs = 0;
    assert(solar_os_jobs_prepare_shutdown(NULL) == ESP_OK);
    assert(stops == 4);
    release_job();
    puts("native job admission/unregister lifetime tests passed");
}
