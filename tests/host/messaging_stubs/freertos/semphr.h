#pragma once
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include "../../freertos/FreeRTOS.h"

typedef pthread_mutex_t StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
#ifndef portMAX_DELAY
#define portMAX_DELAY 0xffffffffU
#endif
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t lock = calloc(1, sizeof(*lock));
    if (lock) assert(pthread_mutex_init(lock, NULL) == 0);
    return lock;
}
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    pthread_mutexattr_t attr;
    assert(pthread_mutexattr_init(&attr) == 0);
    assert(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0);
    SemaphoreHandle_t lock = calloc(1, sizeof(*lock));
    if (lock) assert(pthread_mutex_init(lock, &attr) == 0);
    assert(pthread_mutexattr_destroy(&attr) == 0);
    return lock;
}
static inline int xSemaphoreTake(SemaphoreHandle_t lock, unsigned timeout)
{ (void)timeout; assert(pthread_mutex_lock(lock) == 0); return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t lock)
{ assert(pthread_mutex_unlock(lock) == 0); return 1; }
static inline int xSemaphoreTakeRecursive(SemaphoreHandle_t lock, unsigned timeout)
{ return timeout ? xSemaphoreTake(lock, timeout) : pthread_mutex_trylock(lock) == 0; }
static inline int xSemaphoreGiveRecursive(SemaphoreHandle_t lock)
{ return xSemaphoreGive(lock); }
static inline void vSemaphoreDelete(SemaphoreHandle_t lock)
{ if (lock) { assert(pthread_mutex_destroy(lock) == 0); free(lock); } }
