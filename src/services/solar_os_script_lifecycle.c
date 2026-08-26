#include "solar_os_script_lifecycle.h"

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

bool solar_os_script_wait_for_stop(solar_os_script_stopped_fn stopped,
                                   void *user,
                                   uint32_t timeout_ms,
                                   uint32_t poll_interval_ms)
{
    if (stopped == NULL) {
        return false;
    }
    if (stopped(user)) {
        return true;
    }

    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ticks == 0U) {
        return false;
    }
    TickType_t poll_ticks = pdMS_TO_TICKS(poll_interval_ms);
    if (poll_ticks == 0U) {
        poll_ticks = 1U;
    }

    const TickType_t started = xTaskGetTickCount();
    while (!stopped(user)) {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout_ticks) {
            return false;
        }
        const TickType_t remaining = timeout_ticks - elapsed;
        vTaskDelay(poll_ticks < remaining ? poll_ticks : remaining);
    }
    return true;
}
