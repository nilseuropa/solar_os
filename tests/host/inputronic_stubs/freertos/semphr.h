#pragma once
#include <assert.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
typedef struct { bool taken; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    storage->taken = false;
    return storage;
}
static inline int xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks)
{
    (void)ticks;
    assert(handle != NULL && !handle->taken);
    handle->taken = true;
    return pdTRUE;
}
static inline int xSemaphoreGive(SemaphoreHandle_t handle)
{
    assert(handle != NULL && handle->taken);
    handle->taken = false;
    return pdTRUE;
}
#ifndef portMAX_DELAY
#define portMAX_DELAY UINT32_MAX
#endif
