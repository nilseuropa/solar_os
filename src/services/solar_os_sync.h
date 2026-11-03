#pragma once

#include <stddef.h>
#include <stdbool.h>

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

/* A per-provider transition lock. Metadata registries must release their own
 * locks before entering a transition; callbacks can call other providers. */
typedef struct {
    SemaphoreHandle_t handle;
    StaticSemaphore_t storage;
} solar_os_lifecycle_t;

static inline bool solar_os_lifecycle_enter(solar_os_lifecycle_t *lifecycle)
{
    SemaphoreHandle_t mutex = solar_os_mutex_get(&lifecycle->handle, &lifecycle->storage);
    return mutex != NULL && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
}

static inline void solar_os_lifecycle_leave(solar_os_lifecycle_t *lifecycle)
{
    (void)xSemaphoreGive(lifecycle->handle);
}
