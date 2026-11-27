#pragma once
#include "solar_os_native_service_abi.h"
#include "solar_os_bluetooth_input_types_v1.h"

#define SOLAR_OS_NATIVE_BLE_KEYBOARD_SERVICE "ble.keyboard"
#define SOLAR_OS_NATIVE_BLE_KEYBOARD_ABI 1U

/* Global keyboard policy and copied snapshots. Names are borrowed, immutable
 * core strings. Settings persist according to core policy; boot settings apply
 * on the next boot. Repeat uses the global input repeat limits. Layout setters
 * also maintain legacy keyboard settings for firmware downgrade compatibility.
 * Pairing/forget control the OS keyboard; general peripherals use ble sessions.
 * No callback or application pointers are retained. */
typedef struct {
    uint32_t abi_version, struct_size;
    bool (*enabled_for_current_boot)(void);
    bool (*enabled_for_next_boot)(void);
    bool (*board_default_enabled)(void);
    solar_os_ble_keyboard_boot_setting_t (*boot_setting)(void);
    const char * (*boot_setting_name)(solar_os_ble_keyboard_boot_setting_t setting);
    bool (*parse_boot_setting)(const char *name, solar_os_ble_keyboard_boot_setting_t *setting);
    int (*set_boot_setting)(solar_os_ble_keyboard_boot_setting_t setting);
    int (*set_enabled_for_next_boot)(bool enabled);
    int (*start_pairing)(void);
    int (*forget)(void);
    bool (*is_connected)(void);
    bool (*is_scanning)(void);
    bool (*is_pairing)(void);
    bool (*keepalive_enabled)(void);
    int (*set_keepalive_enabled)(bool enabled);
    int (*get_keepalive_status)(solar_os_native_ble_keyboard_keepalive_v1_t *out);
    const char * (*keepalive_method_name)(solar_os_ble_keyboard_keepalive_method_t method);
    uint32_t (*wake_generation)(void);
    size_t (*remembered_count)(void);
    void (*get_status)(char *buffer, size_t buffer_len);
    size_t (*read_chars)(char *buffer, size_t buffer_len);
    void (*get_key_state)(solar_os_ble_keyboard_key_state_t *state);
    void (*get_repeat)(uint16_t *rate_cps, uint16_t *delay_ms);
    int (*set_repeat)(uint16_t rate_cps, uint16_t delay_ms);
    solar_os_ble_keyboard_layout_t (*layout)(void);
    int (*set_layout)(solar_os_ble_keyboard_layout_t layout);
    const char * (*layout_name)(solar_os_ble_keyboard_layout_t layout);
    bool (*parse_layout)(const char *name, solar_os_ble_keyboard_layout_t *layout);
    const char * (*addr_type_name)(uint8_t addr_type);
    bool (*parse_addr_type)(const char *name, uint8_t *addr_type);
} solar_os_native_ble_keyboard_api_v1_t;
