#pragma once
#include <stdint.h>
#include <pthread.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
#define portMAX_DELAY UINT32_MAX
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t lock, uint32_t ticks);
int xSemaphoreGive(SemaphoreHandle_t lock);
void vSemaphoreDelete(SemaphoreHandle_t lock);
