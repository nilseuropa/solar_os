#pragma once
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

/* Preserve lazy heap allocation for optional services. Allocate outside the
 * initialization lock, publish one winner, and release losing candidates. */
static inline SemaphoreHandle_t solar_os_mutex_get_dynamic(SemaphoreHandle_t *handle)
{
    static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&init_lock);
    SemaphoreHandle_t result = *handle;
    portEXIT_CRITICAL(&init_lock);
    if (result != NULL) return result;
    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    portENTER_CRITICAL(&init_lock);
    if (*handle == NULL) *handle = candidate;
    result = *handle;
    portEXIT_CRITICAL(&init_lock);
    if (candidate != NULL && candidate != result) vSemaphoreDelete(candidate);
    return result;
}
