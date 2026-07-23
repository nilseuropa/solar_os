#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * Default queues live in PSRAM on PSRAM-enabled builds and in internal SRAM on
 * boards without PSRAM. Use the internal variant only for queues accessed from
 * ISRs, while the flash/cache is unavailable, or by timing-critical workers.
 * Creation and deletion functions must be paired.
 */
QueueHandle_t solar_os_queue_create(UBaseType_t length, UBaseType_t item_size);
void solar_os_queue_delete(QueueHandle_t queue);

QueueHandle_t solar_os_queue_create_internal(UBaseType_t length,
                                             UBaseType_t item_size);
void solar_os_queue_delete_internal(QueueHandle_t queue);
