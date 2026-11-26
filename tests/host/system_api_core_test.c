#include "solar_os_native_libc_abi.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "solar_os_native_client.h"
#include "solar_os_system_api.h"
#include "solar_os_memory.h"
#include "solar_os_jobs.h"
#include "solar_os_native_runtime.h"
static unsigned allocations, output_bytes, cleaned;
static unsigned library_started, library_stopped, job_started, job_stopped, job_ticks;
static bool job_running;
static const solar_os_job_t *registered_job;
static const uint32_t scoped[2] = {1, sizeof(scoped)};
void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; void *p = malloc(bytes); if (p) ++allocations; return p; }
void *solar_os_memory_calloc(size_t count, size_t bytes, solar_os_memory_class_t cls, const char *tag)
{ void *p = solar_os_memory_alloc(count * bytes, cls, tag); if (p) memset(p, 0, count * bytes); return p; }
void solar_os_memory_free(void *p) { if (p) --allocations; free(p); }
bool solar_os_memory_register_reclaimer(solar_os_memory_reclaimer_t *reclaimer)
{ return reclaimer && reclaimer->reclaim; }
esp_err_t solar_os_jobs_register_dynamic(const char *id, const char *summary, const solar_os_job_t *job)
{
    assert(!strcmp(id, "core-job") && summary && job && !registered_job);
    registered_job = job;
    return ESP_OK;
}
esp_err_t solar_os_jobs_unregister_dynamic(const char *id, const solar_os_job_t *job)
{
    assert(!strcmp(id, "core-job") && job == registered_job);
    if (job_running) return ESP_ERR_INVALID_STATE;
    registered_job = NULL;
    return ESP_OK;
}
int64_t esp_timer_get_time(void) { return 1234567; }
#include "system_core_providers.inc"
static esp_err_t output(const char *text, size_t size, void *user)
{ assert(user == &output_bytes && size == 4 && !memcmp(text, "core", 4)); output_bytes += size; return ESP_OK; }
static const void *lookup(const char *name, uint32_t abi, uint32_t size, void *user)
{ assert(user == &cleaned); return !strcmp(name, "scope") && abi == 1 && size <= sizeof(scoped) ? scoped : NULL; }
static void cleanup(void *user) { assert(user == &cleaned); ++cleaned; }
static int entry(int argc, char **argv)
{
    assert(argc == 1 && argv && !strcmp(argv[0], "builtin"));
    const solar_os_native_host_api_v1_t *host = solar_os_native_host_v1();
    assert(host->get_service("scope", 1, 8) == scoped && !host->write_utf8("core", 4));
    const solar_os_native_libc_api_v1_t *libc =
        host->get_service(SOLAR_OS_NATIVE_LIBC_SERVICE, SOLAR_OS_NATIVE_LIBC_ABI, sizeof(*libc));
    assert(libc && libc->explicit_bzero);
    uint8_t sensitive[] = {1, 2, 3, 4, 5};
    const uint8_t cleared[] = {1, 0, 0, 0, 5};
    libc->explicit_bzero(sensitive + 1, 3);
    assert(!memcmp(sensitive, cleared, sizeof(sensitive)));
    return 17;
}
static const solar_os_native_library_api_header_v1_t library_table = {1, sizeof(library_table)};
static int library_shutdown(void) { ++library_stopped; return SOLAR_OS_NATIVE_OK; }
static const solar_os_native_library_descriptor_v1_t library_descriptor = {
    1, sizeof(library_descriptor), "core-test", "Core library", &library_table, library_shutdown,
};
static int job_start(int argc, char **argv)
{ assert(argc == 1 && argv); ++job_started; return ESP_OK; }
static void job_stop(void) { ++job_stopped; }
static bool job_tick(uint32_t now) { assert(now == 23); ++job_ticks; return true; }
static const solar_os_native_job_descriptor_v1_t job_descriptor = {
    1, sizeof(job_descriptor), "core-job", "Core job", 25, 25, job_start, job_stop, job_tick,
};
static void *foreign_registration(void *unused)
{
    (void)unused;
    assert(solar_os_native_register_library(&library_descriptor) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(solar_os_native_register_job(&job_descriptor) == ESP_ERR_INVALID_ARG);
    return NULL;
}
static void reject_foreign_registration(void)
{
    pthread_t task;
    assert(!pthread_create(&task, NULL, foreign_registration, NULL));
    assert(!pthread_join(task, NULL));
}
int solar_test_library_entry(int argc, char **argv)
{
    assert(argc == 1 && argv && !strcmp(argv[0], "core-test"));
    reject_foreign_registration();
    ++library_started;
    assert(!solar_os_native_register_library(&library_descriptor));
    assert(solar_os_native_register_library(&library_descriptor) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    return 0;
}
int solar_test_job_entry(int argc, char **argv)
{
    assert(argc == 1 && argv && !strcmp(argv[0], "core-job"));
    reject_foreign_registration();
    assert(!solar_os_native_register_job(&job_descriptor));
    assert(solar_os_native_register_job(&job_descriptor) == ESP_ERR_INVALID_ARG);
    return 0;
}
const solar_os_native_app_manifest_v2_t solar_test_app_manifest = {
    .magic = SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC, .lifecycle_abi = 2,
    .struct_size = sizeof(solar_test_app_manifest), .app_class = SOLAR_OS_NATIVE_APP_CLASS_GUI,
    .worker_stack_bytes = 2048, .worker_priority = 1, .tick_interval_ms = 25,
    .id = "core-ui", .summary = "Compiled test app",
};
int solar_test_app_entry(int argc, char **argv) { return entry(argc, argv); }
int main(void)
{
    const solar_os_native_host_api_v1_t *host = solar_os_native_host_v1();
    assert(host && host->abi_version == 1 && !host->get_service("scope", 1, 8));
    const solar_os_native_time_api_v1_t *clock = solar_os_native_time_get(host->get_service, sizeof(*clock));
    assert(clock && clock->now_us() == 1234567);
    const solar_os_native_thread_api_v1_t *thread = solar_os_native_thread_get(host->get_service, sizeof(*thread));
    void *mutex; assert(thread && !thread->mutex_create(&mutex, 0) && !thread->mutex_lock(mutex));
    assert(!thread->mutex_unlock(mutex) && !thread->mutex_destroy(mutex) && !allocations);
    const solar_os_native_library_service_api_v1_t *library = solar_os_native_library_get(host->get_service, sizeof(*library));
    uint32_t lease = 123; const void *api = (void *)1;
    assert(library && library->acquire("test", 1, 8, &lease, &api) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !lease && !api);
    const solar_os_native_run_options_t options = {.write = output, .write_user = &output_bytes,
        .get_service = lookup, .service_user = &cleaned, .cleanup = cleanup};
    char *argv[] = {"builtin", NULL}; int status;
    assert(!solar_os_native_run_builtin(entry, 1, argv, &options, &status));
    assert(status == 17 && output_bytes == 4 && cleaned == 1 && !host->get_service("scope", 1, 8));
    const solar_os_native_system_api_v1_t *system = solar_os_native_system_get(host->get_service, sizeof(*system));
    assert(system && system->service_count() >= 54 && !allocations);
    const solar_os_native_components_api_v1_t *components = solar_os_native_components_get(host->get_service, sizeof(*components));
    assert(components && components->count() == 3);
    solar_os_native_component_info_v1_t component;
    assert(components->info(3, &component) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !component.id[0]);
    assert(!components->find(0, "core-test", &component) && !strcmp(component.version, "1.0.0"));
    assert(!library->acquire("core-test", 1, sizeof(library_table), &lease, &api));
    assert(library_started == 1 && api == &library_table && lease);
    assert(library->unload("core-test") == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!library->release(lease) && !library->unload("core-test"));
    assert(library_stopped == 1 && !allocations);
    assert(!solar_os_native_module_activate(SOLAR_OS_MODULE_TYPE_JOB, "core-job", NULL, NULL, 0));
    assert(registered_job && solar_os_native_module_active(SOLAR_OS_MODULE_TYPE_JOB, "core-job"));
    char *job_argv[] = {"core-job", NULL};
    assert(!registered_job->start_with_user(registered_job->callback_user, NULL, 1, job_argv));
    job_running = true;
    assert(solar_os_native_module_deactivate(SOLAR_OS_MODULE_TYPE_JOB, "core-job", NULL, 0) == ESP_ERR_INVALID_STATE);
    const solar_os_event_t event = {.type = SOLAR_OS_EVENT_TICK, .data.tick_ms = 23};
    assert(registered_job->event_with_user(registered_job->callback_user, NULL, &event));
    registered_job->stop_with_user(registered_job->callback_user, NULL);
    job_running = false;
    assert(!solar_os_native_module_deactivate(SOLAR_OS_MODULE_TYPE_JOB, "core-job", NULL, 0));
    assert(!registered_job && job_started == 1 && job_stopped == 1 && job_ticks == 1 && !allocations);
    const solar_os_component_t *compiled = solar_os_component_find(SOLAR_OS_MODULE_TYPE_APP, "core-ui");
    assert(compiled);
    solar_os_native_run_options_t admitted = options;
    admitted.expected_manifest = &solar_test_app_manifest;
    assert(solar_os_native_run_builtin(entry, 1, argv, &admitted, &status) == ESP_ERR_INVALID_ARG);
    solar_os_component_t forged = *compiled;
    assert(solar_os_native_run_compiled_app(&forged, 1, argv, &admitted, &status) == ESP_ERR_INVALID_ARG);
    assert(solar_os_native_run_compiled_app(compiled, 1, argv, &options, &status) == ESP_ERR_INVALID_ARG);
    solar_os_native_app_manifest_v2_t changed = solar_test_app_manifest;
    changed.minimum_psram_bytes = 1;
    admitted.expected_manifest = &changed;
    assert(solar_os_native_run_compiled_app(compiled, 1, argv, &admitted, &status) == ESP_ERR_INVALID_ARG);
    assert(cleaned == 1);
    admitted.expected_manifest = &solar_test_app_manifest;
    assert(!solar_os_native_run_compiled_app(compiled, 1, argv, &admitted, &status));
    assert(status == 17 && cleaned == 2 && output_bytes == 8 && !allocations);
    assert(!host->get_service("scope", 1, 8));
    return 0;
}
