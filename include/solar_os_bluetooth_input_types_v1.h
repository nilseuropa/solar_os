#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Canonical Bluetooth keyboard values, independent of transport and SDK. */
#define SOLAR_OS_BLE_KEYBOARD_MAX_PRESSED_KEYS 6U

typedef enum {
    SOLAR_OS_BLE_KEYBOARD_LAYOUT_US,
    SOLAR_OS_BLE_KEYBOARD_LAYOUT_DE,
    SOLAR_OS_BLE_KEYBOARD_LAYOUT_UA,
    SOLAR_OS_BLE_KEYBOARD_LAYOUT_FR,
    SOLAR_OS_BLE_KEYBOARD_LAYOUT_RO,
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
    int32_t last_result; /* Common system ABI result; meaningful if attempted. */
    bool attempted, pending;
} solar_os_native_ble_keyboard_keepalive_v1_t;
