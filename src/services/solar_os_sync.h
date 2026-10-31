#pragma once

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

/* Initialize static mutex storage once, including concurrent first callers.
 * Call on every access; an unlocked handle check can race publication. The
 * initialization lock is local to the translation unit and never spans I/O. */
static inline SemaphoreHandle_t solar_os_mutex_get(SemaphoreHandle_t *handle,
                                                   StaticSemaphore_t *storage)
{
    static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&init_lock);
    if (*handle == NULL) *handle = xSemaphoreCreateMutexStatic(storage);
    SemaphoreHandle_t result = *handle;
    portEXIT_CRITICAL(&init_lock);
    return result;
}
