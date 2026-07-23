#include "solar_os_task.h"

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"

#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM && \
    defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) && \
    CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#define SOLAR_OS_FREERTOS_EXTERNAL_MEMORY 1
#else
#define SOLAR_OS_FREERTOS_EXTERNAL_MEMORY 0
#endif

static void task_log_create_failure(const char *name, uint32_t stack_depth)
{
    solar_os_memory_status_t memory;
    solar_os_memory_get_status(&memory);
    SOLAR_OS_LOGW("task",
                  "create failed name=%s stack=%u internal_free=%u internal_max=%u external_free=%u",
                  name != NULL ? name : "unknown",
                  (unsigned)stack_depth,
                  (unsigned)memory.internal.free,
                  (unsigned)memory.internal.largest_free,
                  (unsigned)memory.external.free);
}

BaseType_t solar_os_task_create_pinned(TaskFunction_t task,
                                        const char *name,
                                        uint32_t stack_depth,
                                        void *parameters,
                                        UBaseType_t priority,
                                        TaskHandle_t *handle,
                                        BaseType_t core_id)
{
    const BaseType_t result = xTaskCreatePinnedToCore(task,
                                                      name,
                                                      stack_depth,
                                                      parameters,
                                                      priority,
                                                      handle,
                                                      core_id);
    if (result != pdPASS) {
        task_log_create_failure(name, stack_depth);
    }
    return result;
}

BaseType_t solar_os_task_create_pinned_external(TaskFunction_t task,
                                                const char *name,
                                                uint32_t stack_depth,
                                                void *parameters,
                                                UBaseType_t priority,
                                                TaskHandle_t *handle,
                                                BaseType_t core_id)
{
#if SOLAR_OS_FREERTOS_EXTERNAL_MEMORY
    const BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        task,
        name,
        stack_depth,
        parameters,
        priority,
        handle,
        core_id,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result != pdPASS) {
        task_log_create_failure(name, stack_depth);
    }
    return result;
#else
    return solar_os_task_create_pinned(task,
                                       name,
                                       stack_depth,
                                       parameters,
                                       priority,
                                       handle,
                                       core_id);
#endif
}

BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t task,
                                                const char *name,
                                                uint32_t stack_depth,
                                                void *parameters,
                                                UBaseType_t priority,
                                                TaskHandle_t *handle,
                                                BaseType_t core_id)
{
    const BaseType_t result = xTaskCreatePinnedToCore(task,
                                                      name,
                                                      stack_depth,
                                                      parameters,
                                                      priority,
                                                      handle,
                                                      core_id);
    if (result != pdPASS) {
        task_log_create_failure(name, stack_depth);
    }
    return result;
}

void solar_os_task_delete(TaskHandle_t task)
{
    vTaskDelete(task);
}

void solar_os_task_delete_external(TaskHandle_t task)
{
#if SOLAR_OS_FREERTOS_EXTERNAL_MEMORY
    vTaskDeleteWithCaps(task);
#else
    vTaskDelete(task);
#endif
}

void solar_os_task_delete_internal(TaskHandle_t task)
{
    vTaskDelete(task);
}

bool solar_os_task_wait_done(TaskHandle_t task,
                             volatile bool *task_done,
                             uint32_t timeout_ms)
{
    if (task == NULL) {
        return true;
    }
    if (task_done != NULL && *task_done) {
        return true;
    }

    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0 && timeout_ticks == 0) {
        timeout_ticks = 1;
    }

    TickType_t poll_ticks = pdMS_TO_TICKS(SOLAR_OS_TASK_STOP_POLL_MS);
    if (poll_ticks == 0) {
        poll_ticks = 1;
    }

    const TickType_t start = xTaskGetTickCount();
    while (task_done == NULL || !*task_done) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (timeout_ticks == 0 || elapsed >= timeout_ticks) {
            return false;
        }

        const TickType_t remaining = timeout_ticks - elapsed;
        vTaskDelay(remaining < poll_ticks ? remaining : poll_ticks);
    }

    return true;
}
