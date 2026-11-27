#include <assert.h>
#include <stdio.h>
#include "solar_os_native_ble_keyboard.h"
#include "solar_os_native_ble_keyboard_abi.h"
#include "ble_keyboard_abi_backend.h"

int main(void)
{
    const solar_os_native_ble_keyboard_api_v1_t *api=solar_os_native_ble_keyboard_get_service("ble.keyboard",1,sizeof(*api));
    assert(api && api->abi_version==1 && api->struct_size==sizeof(*api));
    assert(!solar_os_native_ble_keyboard_get_service("ble.keyboard",2,8));
    assert(!solar_os_native_ble_keyboard_get_service("ble.keyboard",1,sizeof(*api)+1));
    assert(!solar_os_native_ble_keyboard_get_service("ble",1,8));
    assert(api->get_keepalive_status(NULL)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    const esp_err_t errors[]={ESP_OK,ESP_ERR_INVALID_ARG,ESP_ERR_INVALID_STATE,ESP_ERR_NO_MEM,
        ESP_ERR_NOT_FOUND,ESP_ERR_NOT_SUPPORTED,ESP_ERR_TIMEOUT,ESP_ERR_INVALID_SIZE,ESP_FAIL};
    const int results[]={SOLAR_OS_NATIVE_OK,SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT,SOLAR_OS_NATIVE_ERROR_INVALID_STATE,
        SOLAR_OS_NATIVE_ERROR_NO_MEMORY,SOLAR_OS_NATIVE_ERROR_NOT_FOUND,SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED,
        SOLAR_OS_NATIVE_ERROR_TIMEOUT,SOLAR_OS_NATIVE_ERROR_INVALID_SIZE,SOLAR_OS_NATIVE_ERROR_FAILED};
    for(size_t i=0;i<sizeof(errors)/sizeof(errors[0]);++i) {
        keyboard_backend_result=errors[i];
        assert(api->start_pairing()==results[i] && api->forget()==results[i]);
        assert(api->set_boot_setting(SOLAR_OS_BLE_KEYBOARD_BOOT_ENABLED)==results[i]);
        assert(api->set_enabled_for_next_boot(true)==results[i]);
        assert(api->set_keepalive_enabled(true)==results[i]);
        assert(api->set_repeat(20,400)==results[i] && api->set_layout(SOLAR_OS_BLE_KEYBOARD_LAYOUT_DE)==results[i]);
        keyboard_backend_keepalive=(solar_os_ble_keyboard_keepalive_status_t){
            .method=SOLAR_OS_BLE_KEYBOARD_KEEPALIVE_INFORMATION_READ,.attempts=123,.last_status=errors[i],
            .attempted=true,.pending=true};
        solar_os_native_ble_keyboard_keepalive_v1_t snapshot;
        assert(!api->get_keepalive_status(&snapshot));
        keyboard_backend_keepalive=(solar_os_ble_keyboard_keepalive_status_t){0};
        assert(snapshot.last_result==results[i] && snapshot.attempts==123 && snapshot.attempted && snapshot.pending);
        assert(snapshot.method==SOLAR_OS_BLE_KEYBOARD_KEEPALIVE_INFORMATION_READ);
    }
    solar_os_ble_keyboard_key_state_t keys;
    api->get_key_state(&keys);
    assert(keys.connected && keys.modifiers==2 && keys.keycodes[0]==4 && keys.chars[0]=='A');
    char bytes[4]={1,1,1,1};
    assert(api->read_chars(bytes,2)==2 && !bytes[0] && (uint8_t)bytes[1]==255 && bytes[2]==1);
    assert(!api->enabled_for_current_boot() && api->enabled_for_next_boot() && !api->board_default_enabled());
    assert(api->is_connected() && !api->is_scanning() && !api->is_pairing() && api->keepalive_enabled());
    assert(api->wake_generation()==123 && api->remembered_count()==1);
    puts("Bluetooth keyboard ABI: discovery, normalized operation/history errors, copied snapshots and byte boundaries passed");
    return 0;
}
