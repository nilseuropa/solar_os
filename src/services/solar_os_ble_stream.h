#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Runtime-only, exclusively claimed duplex byte streams. The listener uses
 * the shared BLE peripheral lease; outgoing streams retain the keyboard slot.
 * Each claim owns a fresh connection. No peer is replaced within a claim. */
esp_err_t solar_os_ble_stream_listen(const char *name);
esp_err_t solar_os_ble_stream_connect(const char *name, const uint8_t address[6],
                                      uint8_t address_type);
esp_err_t solar_os_ble_stream_remove(const char *name);

typedef struct {
    bool claimed, listening, connected, failed;
    uint16_t rx_mtu, tx_mtu;
    uint32_t connection_interval_us;
    size_t rx_pending, tx_pending;
} solar_os_ble_stream_info_t;
esp_err_t solar_os_ble_stream_status(const char *name, solar_os_ble_stream_info_t *info);

/* SolarOS byte-stream service UUID and readable protocol descriptor UUID.
 * Descriptor: version (1), PSM little-endian (0x0080), receive MTU LE. */
#define SOLAR_OS_BLE_STREAM_SERVICE_UUID "569a2001-b87f-490c-92cb-11ba5ea5167c"
#define SOLAR_OS_BLE_STREAM_INFO_UUID    "569a2002-b87f-490c-92cb-11ba5ea5167c"
#define SOLAR_OS_BLE_STREAM_PSM 0x0080
