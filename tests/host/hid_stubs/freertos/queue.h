#pragma once
#include "FreeRTOS.h"
typedef struct hid_test_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout);
BaseType_t xQueuePeek(QueueHandle_t queue, void *item, TickType_t timeout);
BaseType_t xQueueReset(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);
