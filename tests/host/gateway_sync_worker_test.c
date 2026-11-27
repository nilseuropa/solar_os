#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "solar_os_gateway_sync.h"
#include "solar_os_native_client.h"
#include "solar_os_native_messages.h"

typedef void *TaskHandle_t;
typedef unsigned TickType_t;
typedef bool *SemaphoreHandle_t;
typedef bool StaticSemaphore_t;
typedef int portMUX_TYPE;

#define GATEWAY_SYNC_WORKER_PRIORITY 1
#define GATEWAY_SYNC_STOP_WAIT_MS 3000U
#define GATEWAY_SYNC_STOP_POLL_MS 25U
#define SOLAR_OS_TASK_ROLE_BACKGROUND 1
#define pdPASS 1
#define tskNO_AFFINITY -1
#define pdMS_TO_TICKS(ms) (ms)
#define pdTICKS_TO_MS(ticks) (ticks)
#define portMAX_DELAY UINT32_MAX
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))

static const char runtime_hooks;
static bool fail_worker, complete_on_wait, restart_on_wait;
static TickType_t now_ms;
static unsigned notify_count;

static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *mutex) { return mutex; }
static void xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t delay)
{
    assert(!*mutex && delay == portMAX_DELAY);
    *mutex = true;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex) { assert(*mutex); *mutex = false; }

static esp_err_t solar_os_gateway_instances_init(void)
{
    assert(solar_os_gateway_sync_start() == ESP_ERR_INVALID_STATE);
    assert(solar_os_gateway_sync_stop() == ESP_ERR_INVALID_STATE);
    return ESP_OK;
}
static void solar_os_gateway_set_runtime_hooks(const void *hooks) { assert(hooks == &runtime_hooks); }
static void gateway_sync_worker(void *argument) { (void)argument; }

static int solar_os_task_create_pinned(void (*worker)(void *), const char *name, unsigned stack_size,
    void *argument, unsigned priority, TaskHandle_t *task, int affinity, int role)
{
    assert(worker == gateway_sync_worker && !strcmp(name, "gateway_sync"));
    assert(stack_size == SOLAR_OS_GATEWAY_SYNC_WORKER_STACK && argument == NULL);
    assert(priority == GATEWAY_SYNC_WORKER_PRIORITY && affinity == tskNO_AFFINITY);
    assert(role == SOLAR_OS_TASK_ROLE_BACKGROUND && !solar_os_gateway_sync_running());
    if (fail_worker) return 0;
    *task = &notify_count;
    return pdPASS;
}
static TickType_t xTaskGetTickCount(void) { return now_ms; }
static void vTaskDelay(TickType_t wait);
static void xTaskNotifyGive(TaskHandle_t worker) { assert(worker == &notify_count); ++notify_count; }

#include "gateway_sync_manager.inc"

static void vTaskDelay(TickType_t wait)
{
    now_ms += wait;
    if (complete_on_wait) {
        assert(atomic_load(&gateway_manager.stop_requested));
        complete_on_wait = false;
        gateway_manager_lock();
        gateway_manager.worker = NULL;
        xSemaphoreGive(manager_mutex);
        if (restart_on_wait) {
            restart_on_wait = false;
            assert(!solar_os_gateway_sync_start());
        }
    }
}

int main(void)
{
    const solar_os_native_gateway_sync_api_v1_t *api =
        solar_os_native_gateway_sync_get(solar_os_native_messages_get_service, sizeof(*api));
    assert(api && !api->running() && !api->stop());
    fail_worker = true;
    assert(api->start() == SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !api->running());
    fail_worker = false;
    assert(!api->start() && api->running());
    assert(api->start() == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->stop() == SOLAR_OS_NATIVE_ERROR_TIMEOUT && api->running());
    assert(now_ms == GATEWAY_SYNC_STOP_WAIT_MS && notify_count == 1);
    assert(api->start() == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);

    complete_on_wait = restart_on_wait = true;
    assert(!api->stop() && api->running() && notify_count == 2);
    assert(!atomic_load(&gateway_manager.stop_requested));
    complete_on_wait = true;
    assert(!api->stop() && !api->running() && !api->stop() && notify_count == 3);
    return 0;
}
