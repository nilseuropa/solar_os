#pragma once
#include "freertos/FreeRTOS.h"
typedef int StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffU
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    return storage;
}
void i2c_test_before_lock(void);
static inline int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned timeout)
{
    (void)semaphore;
    (void)timeout;
    i2c_test_before_lock();
    return 1;
}
static inline int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
    return 1;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void);
