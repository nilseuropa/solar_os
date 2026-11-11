#pragma once
#include "freertos/FreeRTOS.h"
typedef unsigned UBaseType_t;
typedef void *QueueHandle_t;
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout);
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout);
