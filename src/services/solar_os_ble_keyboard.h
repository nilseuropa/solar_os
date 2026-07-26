#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_keys.h"

#define SOLAR_OS_BLE_KEYBOARD_NAME_MAX 64
#define SOLAR_OS_BLE_KEYBOARD_SCAN_MAX_RESULTS 32
#define SOLAR_OS_BLE_KEYBOARD_MAX_REMEMBERED 1
#define SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MIN 1U
#define SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MAX 60U
#define SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MIN_MS 100U
#define SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MAX_MS 2000U
#define SOLAR_OS_BLE_GATT_UUID_MAX 37
#define SOLAR_OS_BLE_GATT_MAX_SERVICES 24
#define SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS 64
#define SOLAR_OS_BLE_GATT_VALUE_MAX 128

typedef enum {
    SOLAR_OS_BLE_KEYBOARD_LAYOUT_US,
    SOLAR_OS_BLE_KEYBOARD_LAYOUT_US_INTL,
    SOLAR_OS_BLE_KEYBOARD_LAYOUT_DE,
} solar_os_ble_keyboard_layout_t;

typedef struct {
    uint8_t bda[6];
    uint8_t addr_type;
    int8_t rssi;
    uint16_t appearance;
    bool hid_service;
    bool keyboard_like;
    bool remembered;
    bool connected;
    char name[SOLAR_OS_BLE_KEYBOARD_NAME_MAX];
} solar_os_ble_keyboard_scan_result_t;

typedef struct {
    bool connected;
    uint8_t bda[6];
    uint8_t addr_type;
    uint16_t conn_id;
    uint16_t mtu;
    size_t service_count;
    char status[80];
} solar_os_ble_gatt_status_t;

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
    bool primary;
    char uuid[SOLAR_OS_BLE_GATT_UUID_MAX];
} solar_os_ble_gatt_service_t;

typedef struct {
    uint16_t handle;
    uint8_t properties;
    char uuid[SOLAR_OS_BLE_GATT_UUID_MAX];
} solar_os_ble_gatt_characteristic_t;

esp_err_t solar_os_ble_keyboard_init(void);
esp_err_t solar_os_ble_keyboard_start_pairing(void);
esp_err_t solar_os_ble_keyboard_cancel_pairing(void);
esp_err_t solar_os_ble_keyboard_scan(solar_os_ble_keyboard_scan_result_t *results,
                                     size_t max_results,
                                     size_t *found);
esp_err_t solar_os_ble_keyboard_forget(void);
esp_err_t solar_os_ble_keyboard_prepare_sleep(uint32_t timeout_ms);
void solar_os_ble_keyboard_resume(void);
bool solar_os_ble_keyboard_is_connected(void);
bool solar_os_ble_keyboard_is_scanning(void);
bool solar_os_ble_keyboard_is_pairing(void);
size_t solar_os_ble_keyboard_remembered_count(void);
void solar_os_ble_keyboard_get_status(char *buffer, size_t buffer_len);
size_t solar_os_ble_keyboard_read_chars(char *buffer, size_t buffer_len);
void solar_os_ble_keyboard_get_repeat(uint16_t *rate_cps, uint16_t *delay_ms);
esp_err_t solar_os_ble_keyboard_set_repeat(uint16_t rate_cps, uint16_t delay_ms);
solar_os_ble_keyboard_layout_t solar_os_ble_keyboard_layout(void);
esp_err_t solar_os_ble_keyboard_set_layout(solar_os_ble_keyboard_layout_t layout);
const char *solar_os_ble_keyboard_layout_name(solar_os_ble_keyboard_layout_t layout);
bool solar_os_ble_keyboard_parse_layout(const char *name, solar_os_ble_keyboard_layout_t *layout);
const char *solar_os_ble_keyboard_addr_type_name(uint8_t addr_type);
bool solar_os_ble_keyboard_parse_addr_type(const char *name, uint8_t *addr_type);

esp_err_t solar_os_ble_gatt_connect(const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms);
esp_err_t solar_os_ble_gatt_disconnect(void);
void solar_os_ble_gatt_get_status(solar_os_ble_gatt_status_t *status);
esp_err_t solar_os_ble_gatt_services(solar_os_ble_gatt_service_t *services,
                                     size_t max_services,
                                     size_t *count);
esp_err_t solar_os_ble_gatt_characteristics(size_t service_index,
                                            solar_os_ble_gatt_characteristic_t *characteristics,
                                            size_t max_characteristics,
                                            size_t *count);
esp_err_t solar_os_ble_gatt_read(uint16_t handle,
                                 uint8_t *value,
                                 size_t max_len,
                                 size_t *value_len,
                                 uint32_t timeout_ms);
esp_err_t solar_os_ble_gatt_write(uint16_t handle,
                                  const uint8_t *value,
                                  size_t value_len,
                                  bool with_response,
                                  uint32_t timeout_ms);

typedef void (*solar_os_ble_keyboard_passkey_cb_t)(uint32_t passkey);
void solar_os_ble_keyboard_set_passkey_callback(solar_os_ble_keyboard_passkey_cb_t callback);
void solar_os_ble_keyboard_clear_passkey_callback(void);
