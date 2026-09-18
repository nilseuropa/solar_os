#pragma once

#include "esp_err.h"
#include "esp_hid_common.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/* One OS-owned keyboard. This is not an application GATT session. */
typedef struct {
    uint8_t bda[6];
    uint8_t addr_type;
    atomic_int conn_id;
    atomic_int status;
    atomic_bool connected;
    size_t reports_len;
} solar_os_ble_hid_device_t;

typedef enum {
    SOLAR_OS_BLE_HID_OPEN,
    SOLAR_OS_BLE_HID_CLOSE,
    SOLAR_OS_BLE_HID_INPUT,
    SOLAR_OS_BLE_HID_BATTERY,
} solar_os_ble_hid_event_type_t;

typedef union {
    struct { solar_os_ble_hid_device_t *dev; esp_err_t status; } open;
    struct { solar_os_ble_hid_device_t *dev; esp_err_t status; int reason; } close;
    struct {
        solar_os_ble_hid_device_t *dev;
        esp_hid_usage_t usage;
        uint8_t map_index, report_id;
        uint16_t length;
        uint8_t data[64];
    } input;
    struct { uint8_t level; } battery;
} solar_os_ble_hid_event_t;

typedef void (*solar_os_ble_hid_callback_t)(solar_os_ble_hid_event_type_t type,
                                           solar_os_ble_hid_event_t *event);

typedef enum {
    SOLAR_OS_BLE_HID_KEEPALIVE_UNAVAILABLE,
    SOLAR_OS_BLE_HID_KEEPALIVE_EXIT_SUSPEND,
    SOLAR_OS_BLE_HID_KEEPALIVE_INFORMATION_READ,
} solar_os_ble_hid_keepalive_method_t;

typedef struct {
    solar_os_ble_hid_keepalive_method_t method;
    uint32_t attempts;
    esp_err_t last_status;
    bool attempted;
    bool pending;
} solar_os_ble_hid_keepalive_status_t;

esp_err_t solar_os_ble_hid_init(solar_os_ble_hid_callback_t callback);
esp_err_t solar_os_ble_hid_deinit(void);
solar_os_ble_hid_device_t *solar_os_ble_hid_open(const uint8_t bda[6], uint8_t type);
esp_err_t solar_os_ble_hid_close(solar_os_ble_hid_device_t *dev);
esp_err_t solar_os_ble_hid_keepalive(solar_os_ble_hid_device_t *dev);
void solar_os_ble_hid_get_keepalive_status(solar_os_ble_hid_keepalive_status_t *status);
void solar_os_ble_hid_cancel_open(void);
void solar_os_ble_hid_suspend(void);
bool solar_os_ble_hid_idle(void);
const uint8_t *solar_os_ble_hid_address(solar_os_ble_hid_device_t *dev);
