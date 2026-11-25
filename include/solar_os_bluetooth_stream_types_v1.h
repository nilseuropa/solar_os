#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Copied status of a core-owned named Bluetooth byte-stream endpoint. */
typedef struct {
    bool claimed, listening, connected, failed;
    uint16_t rx_mtu, tx_mtu;
    uint32_t connection_interval_us;
    size_t rx_pending, tx_pending;
} solar_os_ble_stream_info_t;
