#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_native.h"
#include "solar_os_native_app_manifest.h"
#include "solar_os_app_registry.h"
#include "solar_os_memory.h"
#include "native_abi_stubs/freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef struct foreground_slot foreground_slot_t;
typedef struct {
    const foreground_slot_t *slot;
    _Atomic(TaskHandle_t) worker;
    atomic_bool done;
    int argc, exit_code;
    esp_err_t result;
    char args[SOLAR_OS_APP_ARG_MAX][SOLAR_OS_APP_ARG_LEN], detail[160];
} foreground_state_t;
static foreground_slot_t *foreground_slots;
static portMUX_TYPE foreground_slots_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned allocations, dispatches, cleanups, deletions;
static int entry_code;
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
static void solar_os_task_delete_internal(TaskHandle_t task) { assert(!task); ++deletions; }
size_t strlcpy(char *out, const char *text, size_t capacity)
{ size_t size = strlen(text); if (capacity) snprintf(out, capacity, "%s", text); return size; }
void *solar_os_memory_calloc(size_t count, size_t bytes, solar_os_memory_class_t memory_class, const char *tag)
{ (void)memory_class; (void)tag; void *result = calloc(count, bytes); if (result) ++allocations; return result; }
void solar_os_memory_free(void *memory) { if (memory) --allocations; free(memory); }
static esp_err_t foreground_start(solar_os_context_t *ctx) { (void)ctx; return ESP_OK; }
static void foreground_stop(solar_os_context_t *ctx) { (void)ctx; }
static void foreground_suspend(solar_os_context_t *ctx) { (void)ctx; }
static void foreground_resume(solar_os_context_t *ctx) { (void)ctx; }
static bool foreground_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{ (void)ctx; (void)event; return true; }
static esp_err_t foreground_write(const char *text, size_t bytes, void *user)
{ (void)text; (void)bytes; assert(user); return ESP_OK; }
static const void *foreground_get_service(const char *name, uint32_t abi, uint32_t size, void *user)
{ (void)name; (void)abi; (void)size; assert(user); return NULL; }
static void foreground_worker_cleanup(void *user) { assert(user); ++cleanups; }
static int entry(int argc, char **argv) { assert(argc == 1 && !strcmp(argv[0], "compiled")); return entry_code; }
static const solar_os_native_app_manifest_v2_t manifest = {
    .magic = SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC, .lifecycle_abi = 2, .struct_size = sizeof(manifest),
    .app_class = SOLAR_OS_NATIVE_APP_CLASS_GUI, .flags = SOLAR_OS_NATIVE_APP_RESUMABLE,
    .worker_stack_bytes = 2048, .worker_priority = 1, .tick_interval_ms = 25,
    .id = "compiled", .summary = "Compiled foreground test",
};
static const solar_os_component_t component = {.type = SOLAR_OS_MODULE_TYPE_APP, .id = "compiled",
    .name = "Compiled foreground test", .version = "1.0.0", .lifecycle_abi = 2, .entry = entry, .manifest = &manifest};
const solar_os_component_t *solar_os_component_find(solar_os_module_type_t type, const char *id)
{ return (!type || type == SOLAR_OS_MODULE_TYPE_APP) && id && !strcmp(id, component.id) ? &component : NULL; }
esp_err_t solar_os_native_run_compiled_app(const solar_os_component_t *compiled, int argc, char **argv,
    const solar_os_native_run_options_t *options, int *code)
{
    assert(compiled == &component && options->expected_manifest &&
        !memcmp(options->expected_manifest, &manifest, sizeof(manifest)));
    assert(options->get_service == foreground_get_service && options->write == foreground_write);
    assert(options->service_user == options->write_user && options->cleanup == foreground_worker_cleanup);
    ++dispatches;
    *code = compiled->entry(argc, argv);
    options->cleanup(options->service_user);
    return ESP_OK;
}
/* Production metadata publication, discovery and worker dispatch. The core
 * scope runner and resource cleanup have their own integration tests. */
#include "compiled_foreground.inc"

int main(void)
{
    const solar_os_app_t *app = NULL, *again = NULL;
    assert(!solar_os_native_foreground_resolve("compiled", &app, NULL, 0) && app);
    assert(allocations == 1 && app->flags & SOLAR_OS_APP_FLAG_RESUMABLE);
    assert(app->worker_stack_bytes == manifest.worker_stack_bytes);
    assert(!solar_os_native_foreground_resolve("compiled", &again, NULL, 0) && again == app && allocations == 1);
    assert(solar_os_native_foreground_find("compiled")->app == app);
    assert(!solar_os_native_foreground_find("missing"));
    assert(solar_os_native_foreground_resolve("/missing.elf", &again, NULL, 0) == ESP_ERR_NOT_SUPPORTED && !again);
    assert(foreground_prepare_slot("/shadow.elf", &manifest, NULL, &again) == ESP_ERR_INVALID_STATE);
    solar_os_native_app_manifest_v2_t changed = manifest;
    ++changed.worker_stack_bytes;
    assert(foreground_prepare_slot("compiled", &changed, &component, &again) == ESP_ERR_INVALID_STATE);
    changed = manifest; changed.worker_priority = 0;
    assert(foreground_prepare_slot("bad", &changed, NULL, &again) == ESP_ERR_INVALID_RESPONSE);
    changed = manifest; strcpy(changed.id, "other");
    assert(foreground_prepare_slot("compiled", &changed, &component, &again) == ESP_ERR_INVALID_RESPONSE);
    foreground_state_t state = {.slot = foreground_find_slot(app), .argc = 1};
    strcpy(state.args[0], "compiled");
    foreground_worker(&state);
    assert(state.result == ESP_OK && atomic_load(&state.done) && !atomic_load(&state.worker));
    assert(dispatches == 1 && cleanups == 1 && deletions == 1);
    entry_code = 7;
    foreground_worker(&state);
    assert(state.result == ESP_FAIL && strstr(state.detail, "entry returned an error"));
    assert(dispatches == 2 && cleanups == 2 && deletions == 2);
    solar_os_memory_free(foreground_slots);
    assert(!allocations);
    return 0;
}
