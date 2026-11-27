#pragma once
/* Deliberately small fake hardware/persistence boundary for ABI consumers. */
#include "solar_os_ble_keyboard.h"
#include <stdio.h>
#include <string.h>
static esp_err_t keyboard_backend_result;
static solar_os_ble_keyboard_keepalive_status_t keyboard_backend_keepalive;
static solar_os_ble_keyboard_layout_t keyboard_backend_layout=SOLAR_OS_BLE_KEYBOARD_LAYOUT_US;
bool solar_os_ble_keyboard_enabled_for_current_boot(void) { return false; }
bool solar_os_ble_keyboard_enabled_for_next_boot(void) { return true; }
bool solar_os_ble_keyboard_board_default_enabled(void) { return false; }
solar_os_ble_keyboard_boot_setting_t solar_os_ble_keyboard_boot_setting(void) { return SOLAR_OS_BLE_KEYBOARD_BOOT_ENABLED; }
const char *solar_os_ble_keyboard_boot_setting_name(solar_os_ble_keyboard_boot_setting_t setting)
{ (void)setting;return "on"; }
bool solar_os_ble_keyboard_parse_boot_setting(const char *name,solar_os_ble_keyboard_boot_setting_t *out)
{ if(!name || !out || strcmp(name,"on"))return false;*out=SOLAR_OS_BLE_KEYBOARD_BOOT_ENABLED;return true; }
esp_err_t solar_os_ble_keyboard_set_boot_setting(solar_os_ble_keyboard_boot_setting_t setting)
{ (void)setting;return keyboard_backend_result; }
esp_err_t solar_os_ble_keyboard_set_enabled_for_next_boot(bool enabled)
{ (void)enabled;return keyboard_backend_result; }
esp_err_t solar_os_ble_keyboard_start_pairing(void) { return keyboard_backend_result; }
esp_err_t solar_os_ble_keyboard_forget(void) { return keyboard_backend_result; }
bool solar_os_ble_keyboard_is_connected(void) { return true; }
bool solar_os_ble_keyboard_is_scanning(void) { return false; }
bool solar_os_ble_keyboard_is_pairing(void) { return false; }
bool solar_os_ble_keyboard_keepalive_enabled(void) { return true; }
esp_err_t solar_os_ble_keyboard_set_keepalive_enabled(bool enabled)
{ (void)enabled;return keyboard_backend_result; }
void solar_os_ble_keyboard_get_keepalive_status(solar_os_ble_keyboard_keepalive_status_t *out)
{ if(out)*out=keyboard_backend_keepalive; }
const char *solar_os_ble_keyboard_keepalive_method_name(solar_os_ble_keyboard_keepalive_method_t method)
{ (void)method;return "hid-information-read"; }
uint32_t solar_os_ble_keyboard_wake_generation(void) { return 123; }
size_t solar_os_ble_keyboard_remembered_count(void) { return 1; }
void solar_os_ble_keyboard_get_status(char *buffer,size_t capacity)
{ if(buffer && capacity)snprintf(buffer,capacity,"connected"); }
size_t solar_os_ble_keyboard_read_chars(char *buffer,size_t capacity)
{
    const char bytes[]={0,(char)255,'B'};size_t count=capacity<sizeof(bytes)?capacity:sizeof(bytes);
    if(count)memcpy(buffer,bytes,count);
    return count;
}
void solar_os_ble_keyboard_get_key_state(solar_os_ble_keyboard_key_state_t *out)
{ if(out)*out=(solar_os_ble_keyboard_key_state_t){.connected=true,.modifiers=2,.keycodes={4,5},.chars={'A','B'}}; }
void solar_os_ble_keyboard_get_repeat(uint16_t *rate,uint16_t *delay)
{ if(rate)*rate=20;if(delay)*delay=400; }
esp_err_t solar_os_ble_keyboard_set_repeat(uint16_t rate,uint16_t delay)
{ (void)rate;(void)delay;return keyboard_backend_result; }
solar_os_ble_keyboard_layout_t solar_os_ble_keyboard_layout(void) { return keyboard_backend_layout; }
esp_err_t solar_os_ble_keyboard_set_layout(solar_os_ble_keyboard_layout_t layout)
{ if(!keyboard_backend_result)keyboard_backend_layout=layout;return keyboard_backend_result; }
const char *solar_os_ble_keyboard_layout_name(solar_os_ble_keyboard_layout_t layout)
{ return layout==SOLAR_OS_BLE_KEYBOARD_LAYOUT_DE?"de":"us"; }
bool solar_os_ble_keyboard_parse_layout(const char *name,solar_os_ble_keyboard_layout_t *out)
{
    if(!name || !out || strcmp(name,"de"))return false;
    *out=SOLAR_OS_BLE_KEYBOARD_LAYOUT_DE;return true;
}
const char *solar_os_ble_keyboard_addr_type_name(uint8_t type) { (void)type;return "random"; }
bool solar_os_ble_keyboard_parse_addr_type(const char *name,uint8_t *out)
{ if(!name || !out || strcmp(name,"random"))return false;*out=1;return true; }
