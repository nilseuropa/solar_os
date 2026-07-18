#include "solar_os_task.h"

#include "solar_os_log.h"
#include "solar_os_memory.h"

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
    return result;
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
