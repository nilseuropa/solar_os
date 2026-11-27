#pragma once
#include "FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
typedef struct { unsigned unused; } StaticTask_t;
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout);
void xTaskNotifyGive(TaskHandle_t task);
