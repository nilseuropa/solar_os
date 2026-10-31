#pragma once
#include "FreeRTOS.h"
#include <assert.h>
#include <stdatomic.h>
typedef pthread_mutex_t StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
extern atomic_uint registry_mutex_initializations;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s)
{
    assert(pthread_mutex_init(s, NULL) == 0);
    atomic_fetch_add(&registry_mutex_initializations, 1);
    return s;
}
static inline int xSemaphoreTake(SemaphoreHandle_t s, TickType_t timeout)
{
    return (timeout ? pthread_mutex_lock(s) : pthread_mutex_trylock(s)) == 0;
}
static inline int xSemaphoreGive(SemaphoreHandle_t s)
{
    assert(pthread_mutex_unlock(s) == 0);
    return 1;
}
