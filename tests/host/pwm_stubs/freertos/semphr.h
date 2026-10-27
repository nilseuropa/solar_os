#pragma once
#include <assert.h>
#include <pthread.h>
#include "freertos/FreeRTOS.h"
typedef pthread_mutex_t StaticSemaphore_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
#define portMAX_DELAY UINT32_MAX
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    assert(pthread_mutex_init(storage, NULL) == 0);
    return storage;
}
static inline int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout)
{
    (void)timeout;
    assert(pthread_mutex_lock(mutex) == 0);
    return pdTRUE;
}
static inline int xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(pthread_mutex_unlock(mutex) == 0);
    return pdTRUE;
}
static inline void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    assert(pthread_mutex_destroy(mutex) == 0);
}
