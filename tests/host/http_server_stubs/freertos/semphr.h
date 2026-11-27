#pragma once

#include "freertos/FreeRTOS.h"

typedef pthread_mutex_t StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;

#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1

static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    return pthread_mutex_init(storage, NULL) == 0 ? storage : NULL;
}

static inline int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout)
{
    (void)timeout;
    return pthread_mutex_lock(mutex) == 0;
}

static inline int xSemaphoreGive(SemaphoreHandle_t mutex)
{
    return pthread_mutex_unlock(mutex) == 0;
}
