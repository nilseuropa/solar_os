#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_ble.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"

#define SOLAR_OS_BLE_KEYBOARD_NAME_MAX SOLAR_OS_BLE_NAME_MAX
#define SOLAR_OS_BLE_KEYBOARD_SCAN_MAX_RESULTS SOLAR_OS_BLE_SCAN_MAX_RESULTS
#define SOLAR_OS_BLE_KEYBOARD_MAX_REMEMBERED 1
#define SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MIN SOLAR_OS_INPUT_REPEAT_RATE_MIN
#define SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MAX SOLAR_OS_INPUT_REPEAT_RATE_MAX
#define SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MIN_MS SOLAR_OS_INPUT_REPEAT_DELAY_MIN_MS
#define SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MAX_MS SOLAR_OS_INPUT_REPEAT_DELAY_MAX_MS
#define SOLAR_OS_BLE_KEYBOARD_MAX_PRESSED_KEYS 6U
#define SOLAR_OS_BLE_KEYBOARD_KEEPALIVE_INTERVAL_MS 30000U

typedef enum {
    SOLAR_OS_BLE_KEYBOARD_LAYOUT_US,
    SOLAR_OS_BLE_KEYBOARD_LAYOUT_DE,
} solar_os_ble_keyboard_layout_t;

typedef enum {
    SOLAR_OS_BLE_KEYBOARD_BOOT_DEFAULT,
    SOLAR_OS_BLE_KEYBOARD_BOOT_ENABLED,
    SOLAR_OS_BLE_KEYBOARD_BOOT_DISABLED,
} solar_os_ble_keyboard_boot_setting_t;

typedef struct {
    bool connected;
    uint8_t modifiers;
    uint8_t keycodes[SOLAR_OS_BLE_KEYBOARD_MAX_PRESSED_KEYS];
    uint8_t chars[SOLAR_OS_BLE_KEYBOARD_MAX_PRESSED_KEYS];
} solar_os_ble_keyboard_key_state_t;

typedef enum {
    SOLAR_OS_BLE_KEYBOARD_KEEPALIVE_UNAVAILABLE,
    SOLAR_OS_BLE_KEYBOARD_KEEPALIVE_EXIT_SUSPEND,
    SOLAR_OS_BLE_KEYBOARD_KEEPALIVE_INFORMATION_READ,
} solar_os_ble_keyboard_keepalive_method_t;

typedef struct {
    solar_os_ble_keyboard_keepalive_method_t method;
    uint32_t attempts;
    esp_err_t last_status;
    bool attempted;
    bool pending;
} solar_os_ble_keyboard_keepalive_status_t;

/* Compatibility alias; generic scanning and GATT live in solar_os_ble.h. */
typedef solar_os_ble_scan_result_t solar_os_ble_keyboard_scan_result_t;

esp_err_t solar_os_ble_keyboard_init(void);
esp_err_t solar_os_ble_keyboard_apply_boot_policy(void);
bool solar_os_ble_keyboard_enabled_for_current_boot(void);
bool solar_os_ble_keyboard_enabled_for_next_boot(void);
bool solar_os_ble_keyboard_board_default_enabled(void);
solar_os_ble_keyboard_boot_setting_t solar_os_ble_keyboard_boot_setting(void);
const char *solar_os_ble_keyboard_boot_setting_name(
    solar_os_ble_keyboard_boot_setting_t setting);
bool solar_os_ble_keyboard_parse_boot_setting(
    const char *name,
    solar_os_ble_keyboard_boot_setting_t *setting);
esp_err_t solar_os_ble_keyboard_set_boot_setting(
    solar_os_ble_keyboard_boot_setting_t setting);
esp_err_t solar_os_ble_keyboard_set_enabled_for_next_boot(bool enabled);
esp_err_t solar_os_ble_keyboard_start_pairing(void);
esp_err_t solar_os_ble_keyboard_scan(solar_os_ble_keyboard_scan_result_t *results,
                                     size_t max_results,
                                     size_t *found);
esp_err_t solar_os_ble_keyboard_forget(void);
esp_err_t solar_os_ble_keyboard_prepare_sleep(uint32_t timeout_ms);
bool solar_os_ble_keyboard_sleep_prepare_ready(void);
void solar_os_ble_keyboard_resume(void);
bool solar_os_ble_keyboard_is_connected(void);
bool solar_os_ble_keyboard_is_scanning(void);
bool solar_os_ble_keyboard_is_pairing(void);
bool solar_os_ble_keyboard_keepalive_enabled(void);
esp_err_t solar_os_ble_keyboard_set_keepalive_enabled(bool enabled);
void solar_os_ble_keyboard_get_keepalive_status(
    solar_os_ble_keyboard_keepalive_status_t *status);
const char *solar_os_ble_keyboard_keepalive_method_name(
    solar_os_ble_keyboard_keepalive_method_t method);
void solar_os_ble_keyboard_poll(uint32_t now_ms);
size_t solar_os_ble_keyboard_remembered_count(void);
void solar_os_ble_keyboard_get_status(char *buffer, size_t buffer_len);
size_t solar_os_ble_keyboard_read_chars(char *buffer, size_t buffer_len);
void solar_os_ble_keyboard_get_key_state(solar_os_ble_keyboard_key_state_t *state);
void solar_os_ble_keyboard_get_repeat(uint16_t *rate_cps, uint16_t *delay_ms);
esp_err_t solar_os_ble_keyboard_set_repeat(uint16_t rate_cps, uint16_t delay_ms);
solar_os_ble_keyboard_layout_t solar_os_ble_keyboard_layout(void);
esp_err_t solar_os_ble_keyboard_set_layout(solar_os_ble_keyboard_layout_t layout);
const char *solar_os_ble_keyboard_layout_name(solar_os_ble_keyboard_layout_t layout);
bool solar_os_ble_keyboard_parse_layout(const char *name, solar_os_ble_keyboard_layout_t *layout);
const char *solar_os_ble_keyboard_addr_type_name(uint8_t addr_type);
bool solar_os_ble_keyboard_parse_addr_type(const char *name, uint8_t *addr_type);
