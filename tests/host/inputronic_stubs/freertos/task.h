#pragma once
#include "freertos/FreeRTOS.h"
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdPASS 1
#define tskIDLE_PRIORITY 0
#define tskNO_AFFINITY (-1)
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *wake);
#define portYIELD_FROM_ISR() ((void)0)

TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
