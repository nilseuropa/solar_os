#pragma once
#include "FreeRTOS.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
typedef pthread_mutex_t StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
extern atomic_uint registry_mutex_initializations;
extern atomic_uint registry_dynamic_live;
extern pthread_barrier_t *registry_dynamic_create_barrier;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t s = malloc(sizeof(*s));
    if (s) {
        assert(pthread_mutex_init(s, NULL) == 0);
        atomic_fetch_add(&registry_dynamic_live, 1);
        if (registry_dynamic_create_barrier) pthread_barrier_wait(registry_dynamic_create_barrier);
    }
    return s;
}
static inline void vSemaphoreDelete(SemaphoreHandle_t s)
{
    assert(pthread_mutex_destroy(s) == 0);
    atomic_fetch_sub(&registry_dynamic_live, 1);
    free(s);
}
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
