#pragma once
#include <pthread.h>
#include <stdlib.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *m = malloc(sizeof(*m));
    if (m) pthread_mutex_init(m, NULL);
    return m;
}
static inline int xSemaphoreTake(SemaphoreHandle_t m, unsigned ticks)
{ (void)ticks; return pthread_mutex_lock(m) == 0; }
static inline void xSemaphoreGive(SemaphoreHandle_t m) { pthread_mutex_unlock(m); }
static inline void vSemaphoreDelete(SemaphoreHandle_t m) { pthread_mutex_destroy(m); free(m); }
