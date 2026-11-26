#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
typedef int BaseType_t;
typedef struct { bool binary, set; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
#define portMAX_DELAY UINT32_MAX
#define portYIELD_FROM_ISR() ((void)0)
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{ return storage; }
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
void xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *woken);
