#pragma once
#include "FreeRTOS.h"
void vTaskDelay(TickType_t);

TaskHandle_t xTaskGetCurrentTaskHandle(void);
