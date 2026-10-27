#include <assert.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <freertos/semphr.h>
#include "esp_err.h"

void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    assert(pthread_cond_destroy(&sem->changed) == 0);
    assert(pthread_mutex_destroy(&sem->lock) == 0);
}

TickType_t xTaskGetTickCount(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (TickType_t)((uint64_t)now.tv_sec * 1000U + now.tv_nsec / 1000000U);
}

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *sem)
{
    assert(pthread_mutex_init(&sem->lock, NULL) == 0);
    assert(pthread_cond_init(&sem->changed, NULL) == 0);
    sem->count = 0;
    sem->mutex = false;
    sem->recursive = false;
    sem->depth = 0;
    return sem;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    StaticSemaphore_t *sem = malloc(sizeof(*sem));
    return sem ? xSemaphoreCreateMutexStatic(sem) : NULL;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *sem)
{
    xSemaphoreCreateBinaryStatic(sem);
    sem->count = 1;
    sem->mutex = true;
    return sem;
}

int xSemaphoreTake(SemaphoreHandle_t sem, unsigned timeout)
{
    assert(sem != NULL);
    pthread_mutex_lock(&sem->lock);
    if (sem->mutex && sem->count == 0) {
        if (sem->recursive && pthread_equal(sem->owner, pthread_self())) {
            ++sem->depth;
            pthread_mutex_unlock(&sem->lock);
            return pdTRUE;
        }
        assert(!pthread_equal(sem->owner, pthread_self()));
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout / 1000;
    deadline.tv_nsec += (long)(timeout % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    while (sem->count == 0 && timeout != 0) {
        int ret = timeout == portMAX_DELAY ? pthread_cond_wait(&sem->changed, &sem->lock) :
            pthread_cond_timedwait(&sem->changed, &sem->lock, &deadline);
        if (ret == ETIMEDOUT) {
            break;
        }
        assert(ret == 0);
    }
    const bool success = sem->count != 0;
    if (success) {
        sem->count = 0;
        sem->owner = pthread_self();
        sem->depth = 1;
    }
    pthread_mutex_unlock(&sem->lock);
    return success ? pdTRUE : pdFALSE;
}

int xSemaphoreGive(SemaphoreHandle_t sem)
{
    assert(sem != NULL);
    pthread_mutex_lock(&sem->lock);
    if (sem->mutex) {
        assert(sem->count == 0 && pthread_equal(sem->owner, pthread_self()));
        if (sem->recursive && --sem->depth) {
            pthread_mutex_unlock(&sem->lock);
            return pdTRUE;
        }
    }
    sem->count = 1;
    pthread_cond_signal(&sem->changed);
    pthread_mutex_unlock(&sem->lock);
    return pdTRUE;
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *sem)
{
    xSemaphoreCreateMutexStatic(sem);
    sem->recursive = true;
    return sem;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    StaticSemaphore_t *sem = malloc(sizeof(*sem));
    assert(sem);
    return xSemaphoreCreateBinaryStatic(sem);
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size != 0) {
        const size_t copied = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copied);
        dst[copied] = '\0';
    }
    return len;
}

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "test error";
}
