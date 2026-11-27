#include "solar_os_native_ble_keyboard.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_ble_keyboard_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_SERVICE_BLE
#include "solar_os_ble_keyboard.h"
static int keyboard_set_boot_setting(solar_os_ble_keyboard_boot_setting_t setting)
{ return solar_os_native_result(solar_os_ble_keyboard_set_boot_setting(setting)); }
static int keyboard_set_enabled_for_next_boot(bool enabled)
{ return solar_os_native_result(solar_os_ble_keyboard_set_enabled_for_next_boot(enabled)); }
static int keyboard_start_pairing(void)
{ return solar_os_native_result(solar_os_ble_keyboard_start_pairing()); }
static int keyboard_forget(void)
{ return solar_os_native_result(solar_os_ble_keyboard_forget()); }
static int keyboard_set_keepalive_enabled(bool enabled)
{ return solar_os_native_result(solar_os_ble_keyboard_set_keepalive_enabled(enabled)); }
static int keyboard_get_keepalive_status(solar_os_native_ble_keyboard_keepalive_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_ble_keyboard_keepalive_status_t status = {0};
    solar_os_ble_keyboard_get_keepalive_status(&status);
    *out = (solar_os_native_ble_keyboard_keepalive_v1_t){.method = status.method,
        .attempts = status.attempts, .last_result = solar_os_native_result(status.last_status),
        .attempted = status.attempted, .pending = status.pending};
    return SOLAR_OS_NATIVE_OK;
}
static int keyboard_set_repeat(uint16_t rate_cps, uint16_t delay_ms)
{ return solar_os_native_result(solar_os_ble_keyboard_set_repeat(rate_cps, delay_ms)); }
static int keyboard_set_layout(solar_os_ble_keyboard_layout_t layout)
{ return solar_os_native_result(solar_os_ble_keyboard_set_layout(layout)); }
static const solar_os_native_ble_keyboard_api_v1_t ble_keyboard_api = {
    .abi_version = SOLAR_OS_NATIVE_BLE_KEYBOARD_ABI, .struct_size = sizeof(ble_keyboard_api),
    .enabled_for_current_boot = solar_os_ble_keyboard_enabled_for_current_boot,
    .enabled_for_next_boot = solar_os_ble_keyboard_enabled_for_next_boot,
    .board_default_enabled = solar_os_ble_keyboard_board_default_enabled,
    .boot_setting = solar_os_ble_keyboard_boot_setting,
    .boot_setting_name = solar_os_ble_keyboard_boot_setting_name,
    .parse_boot_setting = solar_os_ble_keyboard_parse_boot_setting,
    .set_boot_setting = keyboard_set_boot_setting,
    .set_enabled_for_next_boot = keyboard_set_enabled_for_next_boot,
    .start_pairing = keyboard_start_pairing,
    .forget = keyboard_forget,
    .is_connected = solar_os_ble_keyboard_is_connected,
    .is_scanning = solar_os_ble_keyboard_is_scanning,
    .is_pairing = solar_os_ble_keyboard_is_pairing,
    .keepalive_enabled = solar_os_ble_keyboard_keepalive_enabled,
    .set_keepalive_enabled = keyboard_set_keepalive_enabled,
    .get_keepalive_status = keyboard_get_keepalive_status,
    .keepalive_method_name = solar_os_ble_keyboard_keepalive_method_name,
    .wake_generation = solar_os_ble_keyboard_wake_generation,
    .remembered_count = solar_os_ble_keyboard_remembered_count,
    .get_status = solar_os_ble_keyboard_get_status,
    .read_chars = solar_os_ble_keyboard_read_chars,
    .get_key_state = solar_os_ble_keyboard_get_key_state,
    .get_repeat = solar_os_ble_keyboard_get_repeat,
    .set_repeat = keyboard_set_repeat,
    .layout = solar_os_ble_keyboard_layout,
    .set_layout = keyboard_set_layout,
    .layout_name = solar_os_ble_keyboard_layout_name,
    .parse_layout = solar_os_ble_keyboard_parse_layout,
    .addr_type_name = solar_os_ble_keyboard_addr_type_name,
    .parse_addr_type = solar_os_ble_keyboard_parse_addr_type,
};
#endif
const void *solar_os_native_ble_keyboard_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_ble_keyboard_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
