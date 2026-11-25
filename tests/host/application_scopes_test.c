#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_log.h"
#include "solar_os_shell_io.h"
#include "freertos/task.h"

static const solar_os_app_t parent, child;
static const solar_os_application_lifecycle_api_v1_t portable;
static solar_os_context_t contexts[2];
static unsigned allocations, starts, stops, cleaned, readiness_checks, resumes, suspends, titles;
static atomic_uint events;
static bool ready = true, start_failure, start_deferred;
static unsigned pending_delays, cleanup_delays;
static void *cold;
bool solar_os_shutdown_requested(void) { return false; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test error"; }
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *format, ...)
{ (void)level; (void)tag; (void)format; return ESP_OK; }
void solar_os_shell_io_capture_output(solar_os_shell_io_t *io, solar_os_context_t *ctx)
{ (void)io; (void)ctx; }
void *solar_os_memory_calloc(size_t count, size_t bytes, solar_os_memory_class_t memory_class, const char *tag)
{ assert(memory_class == SOLAR_OS_MEMORY_TRANSIENT && !strcmp(tag, "parent")); ++allocations; return calloc(count, bytes); }
void solar_os_memory_free(void *pointer) { if (pointer) --allocations; free(pointer); }

static void owner(const solar_os_app_t *app, solar_os_context_t *ctx)
{ assert(solar_os_application_context() == ctx && solar_os_application_descriptor() == app); }
void vTaskDelay(TickType_t ticks)
{
    /* Reboot has a separate, existing presentation delay. */
    if (ticks == pdMS_TO_TICKS(150)) return;
    assert(ticks == 1 && pending_delays && cold && allocations == 1);
    owner(&child, &contexts[1]);
    ++cleanup_delays;
    ready = --pending_delays == 0;
}
static esp_err_t start(solar_os_context_t *ctx)
{
    owner(&parent, ctx); assert(cold); ++starts;
    if (start_deferred) ready = false;
    return start_failure ? ESP_FAIL : ESP_OK;
}
static void stop(solar_os_context_t *ctx) { owner(&parent, ctx); ++stops; }
static bool release_ready(void) { owner(&parent, &contexts[0]); ++readiness_checks; return ready; }
static void release_cleanup(void) { owner(&parent, &contexts[0]); assert(cold); ++cleaned; }
static bool child_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{ owner(&child, ctx); assert(event && event->type == SOLAR_OS_EVENT_TICK); ++events; return true; }
static void *foreign(void *user)
{
    assert(!user && !solar_os_application_context() && !solar_os_application_descriptor());
    const solar_os_event_t event = {.type = SOLAR_OS_EVENT_TICK};
    assert(solar_os_app_event(&child, &contexts[1], &event));
    assert(!solar_os_application_context() && !solar_os_application_descriptor());
    return NULL;
}
static bool parent_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    owner(&parent, ctx);
    assert(solar_os_app_event(&child, &contexts[1], event));
    owner(&parent, ctx);
    pthread_t task;
    assert(!pthread_create(&task, NULL, foreign, NULL) && !pthread_join(task, NULL));
    owner(&parent, ctx);
    return true;
}
static void resume(solar_os_context_t *ctx) { owner(&parent, ctx); ++resumes; }
static void suspend(solar_os_context_t *ctx) { owner(&parent, ctx); ++suspends; }
static void title(solar_os_context_t *ctx, char *out, size_t size)
{ owner(&parent, ctx); snprintf(out, size, "parent"); ++titles; }
static const solar_os_app_t parent = {
    .name = "parent", .app_class = SOLAR_OS_APP_CLASS_COMMAND, .start = start, .stop = stop,
    .event = parent_event, .resume = resume, .suspend = suspend, .title = title,
    .state_slot = &cold, .state_size = 16, .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .state_release_ready = release_ready, .state_release_cleanup = release_cleanup,
};
static const solar_os_app_t child = {.name = "child", .app_class = SOLAR_OS_APP_CLASS_COMMAND, .event = child_event};

static int portable_status;
static int portable_start(solar_os_application_context_t *ctx)
{ owner(&portable, ctx); assert(cold); ++starts; return portable_status; }
static bool portable_ready(void)
{ owner(&portable, &contexts[0]); return ready; }
static void portable_cleanup(void)
{ owner(&portable, &contexts[0]); assert(cold); ++cleaned; }
static uint32_t portable_interval(void)
{
    assert(!solar_os_application_context() && solar_os_application_descriptor() == &portable);
    return cold ? 7 : 0;
}
static const solar_os_application_lifecycle_api_v1_t portable = {
    .abi_version = SOLAR_OS_APPLICATION_LIFECYCLE_ABI, .struct_size = sizeof(portable),
    .name = "parent", .app_class = SOLAR_OS_APP_CLASS_COMMAND, .start = portable_start,
    .state_slot = &cold, .state_size = 16, .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .state_release_ready = portable_ready, .state_release_cleanup = portable_cleanup,
    .tick_interval_ms = 11, .requested_tick_interval_ms = portable_interval,
};
static int legacy_start_status(solar_os_context_t *ctx) { (void)ctx; return ESP_ERR_TIMEOUT; }
static void portable_contract(void)
{
    solar_os_application_scope_t outer;
    solar_os_application_scope_enter(&outer, &child, &contexts[1]);
    const unsigned before = starts;
    solar_os_app_t invalid = portable;
    invalid.abi_version = 99;
    assert(solar_os_app_start(&invalid, &contexts[0]) == ESP_ERR_NOT_SUPPORTED);
    invalid = portable; invalid.struct_size = 8;
    assert(solar_os_app_start(&invalid, &contexts[0]) == ESP_ERR_INVALID_SIZE);
    invalid = portable; invalid.state_storage = (solar_os_application_state_storage_v1_t)99;
    assert(solar_os_app_start(&invalid, &contexts[0]) == ESP_ERR_INVALID_ARG);
    assert(starts == before && !cold && !allocations);
    owner(&child, &contexts[1]);
    assert(solar_os_app_tick_interval_ms(&portable, 25) == 11);
    owner(&child, &contexts[1]);
    portable_status = SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    assert(solar_os_app_start(&portable, &contexts[0]) == ESP_ERR_NOT_SUPPORTED && !cold && !allocations);
    owner(&child, &contexts[1]);
    portable_status = SOLAR_OS_NATIVE_OK;
    assert(!solar_os_app_start(&portable, &contexts[0]) && cold && allocations == 1);
    assert(solar_os_app_tick_interval_ms(&portable, 25) == 7);
    owner(&child, &contexts[1]);
    ready = false;
    assert(!solar_os_app_stop_checked(&portable, &contexts[0]) && cold && allocations == 1);
    ready = true;
    assert(solar_os_app_stop_checked(&portable, &contexts[0]) && !cold && !allocations);
    owner(&child, &contexts[1]);
    const solar_os_app_t legacy = {.name="legacy", .app_class=SOLAR_OS_APP_CLASS_COMMAND, .start=legacy_start_status};
    assert(solar_os_app_start(&legacy, &contexts[0]) == ESP_ERR_TIMEOUT);
    owner(&child, &contexts[1]);
    solar_os_application_scope_leave(&outer);
    assert(!solar_os_application_context() && !solar_os_application_descriptor());
}

static void deferred_cleanup_contract(void)
{
    solar_os_application_scope_t outer;
    solar_os_application_scope_enter(&outer, &child, &contexts[1]);
    unsigned before = stops, before_cleanup = cleaned;
    pending_delays = 2; start_failure = start_deferred = true;
    assert(solar_os_app_start(&parent, &contexts[0]) == ESP_FAIL);
    assert(!cold && !allocations && !pending_delays && cleanup_delays == 2);
    assert(stops == before + 1 && cleaned == before_cleanup + 1);
    owner(&child, &contexts[1]);
    start_failure = start_deferred = false;
    assert(!solar_os_app_start(&parent, &contexts[0]));
    before = stops; before_cleanup = cleaned;
    ready = false; pending_delays = 3;
    solar_os_app_stop(&parent, &contexts[0]);
    assert(!cold && !allocations && !pending_delays && cleanup_delays == 5);
    assert(stops == before + 1 && cleaned == before_cleanup + 1);
    owner(&child, &contexts[1]);
    solar_os_application_scope_leave(&outer);
    assert(!solar_os_application_context() && !solar_os_application_descriptor());
}

int main(void)
{
    assert(!solar_os_application_context() && !solar_os_application_descriptor());
    assert(!solar_os_app_start(&parent, &contexts[0]) && starts == 1 && allocations == 1);
    assert(!solar_os_application_context());
    const solar_os_event_t event = {.type = SOLAR_OS_EVENT_TICK};
    assert(solar_os_app_event(&parent, &contexts[0], &event) && events == 2 && allocations == 1);
    solar_os_app_suspend(&parent, &contexts[0]);
    solar_os_app_resume(&parent, &contexts[0]);
    char text[16]; solar_os_app_title(&parent, &contexts[0], text, sizeof(text));
    assert(suspends == 1 && resumes == 1 && titles == 1 && !strcmp(text, "parent"));
    assert(!solar_os_application_context());
    ready = false;
    assert(!solar_os_app_stop_checked(&parent, &contexts[0]) && allocations == 1 && cold && !cleaned);
    assert(!solar_os_application_context());
    ready = true;
    assert(solar_os_app_stop_checked(&parent, &contexts[0]) && !allocations && !cold && cleaned == 1);
    assert(stops == 2 && readiness_checks == 2 && !solar_os_application_descriptor());
    solar_os_application_scope_t outer;
    solar_os_application_scope_enter(&outer, &child, &contexts[1]);
    start_failure = true;
    assert(solar_os_app_start(&parent, &contexts[0]) == ESP_FAIL && !cold && !allocations && cleaned == 2);
    owner(&child, &contexts[1]);
    solar_os_app_suspend(NULL, NULL); solar_os_app_resume(NULL, NULL);
    assert(!solar_os_app_event(NULL, NULL, NULL));
    owner(&child, &contexts[1]);
    solar_os_application_scope_leave(&outer);
    assert(!solar_os_application_context() && !solar_os_application_descriptor());
    portable_contract();
    deferred_cleanup_contract();
    return 0;
}
