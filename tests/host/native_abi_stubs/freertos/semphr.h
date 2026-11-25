#pragma once
#include "FreeRTOS.h"
#include <stdlib.h>
typedef pthread_mutex_t StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffU
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{ return pthread_mutex_init(storage, NULL) ? NULL : storage; }
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{ SemaphoreHandle_t mutex = malloc(sizeof(*mutex));
  if (mutex && !xSemaphoreCreateMutexStatic(mutex)) { free(mutex); return NULL; } return mutex; }
static inline int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned timeout)
{ (void)timeout; return !pthread_mutex_lock(mutex); }
static inline int xSemaphoreGive(SemaphoreHandle_t mutex) { return !pthread_mutex_unlock(mutex); }
