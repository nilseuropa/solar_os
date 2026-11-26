#pragma once

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>
#include "freertos/FreeRTOS.h"

#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool occupied, signal;
} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;

extern atomic_uint speech_test_waiters;

static inline SemaphoreHandle_t test_semaphore(StaticSemaphore_t *sem, bool signal)
{
    assert(!pthread_mutex_init(&sem->lock, NULL));
    assert(!pthread_cond_init(&sem->changed, NULL));
    sem->signal = signal;
    sem->occupied = !signal;
    return sem;
}

static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *sem)
{ return test_semaphore(sem, false); }

static inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *sem)
{ return test_semaphore(sem, true); }

static inline int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout)
{
    assert(!pthread_mutex_lock(&sem->lock));
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout / 1000;
    deadline.tv_nsec += (timeout % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    while (!sem->occupied) {
        if (sem->signal) atomic_fetch_add(&speech_test_waiters, 1);
        int error = timeout == portMAX_DELAY ? pthread_cond_wait(&sem->changed, &sem->lock) :
            pthread_cond_timedwait(&sem->changed, &sem->lock, &deadline);
        if (sem->signal) atomic_fetch_sub(&speech_test_waiters, 1);
        if (error == ETIMEDOUT) {
            assert(!pthread_mutex_unlock(&sem->lock));
            return pdFALSE;
        }
        assert(!error);
    }
    sem->occupied = false;
    assert(!pthread_mutex_unlock(&sem->lock));
    return pdTRUE;
}

static inline int xSemaphoreGive(SemaphoreHandle_t sem)
{
    assert(!pthread_mutex_lock(&sem->lock));
    const bool available = !sem->occupied;
    if (available) {
        sem->occupied = true;
        assert(!pthread_cond_signal(&sem->changed));
    }
    assert(!pthread_mutex_unlock(&sem->lock));
    return available ? pdTRUE : pdFALSE;
}
