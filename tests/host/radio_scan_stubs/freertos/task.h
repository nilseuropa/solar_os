#pragma once
#include "FreeRTOS.h"
#include "../../epd_scan_stubs/freertos/task.h"
void vTaskDelay(TickType_t ticks);
void vTaskSuspend(TaskHandle_t task);
typedef enum { eRunning, eSuspended } eTaskState;
eTaskState eTaskGetState(TaskHandle_t task);
