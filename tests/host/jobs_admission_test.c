/* Actual registry implementation; deterministic interleavings at unlocked
 * preflight boundaries expose descriptor/pending lifetime errors under ASan. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_jobs.h"
#include "solar_os_task.h"
#include "solar_os_memory.h"
#include "solar_os_log.h"
#include "jobs/solar_os_job_registry.h"

static unsigned depth, exit_count, hook_exit;
static void (*exit_hook)(void);
static bool can_create = true;
static int allocations, starts, stops;
static solar_os_job_t *descriptor;
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
static void register_job(void)
{
    descriptor = calloc(1,sizeof(*descriptor)); assert(descriptor);
    *descriptor = (solar_os_job_t){.name="dynamic",.summary="admission test",
        .kind=SOLAR_OS_JOB_KIND_BACKGROUND,.start=start,.stop=stop,.worker_stack_bytes=4096};
    assert(!solar_os_jobs_register_dynamic("dynamic",descriptor->summary,descriptor));
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
    register_job(); assert(!solar_os_jobs_start(NULL,"dynamic",0,NULL));
    assert(starts == 1); release_job(); assert(stops == 1 && !allocations);
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
    puts("native job admission/unregister lifetime tests passed");
}
