#define SOLAR_OS_HID_SERVICE_FIXTURE
#include "hid_service_test.c"
#include "solar_os_native_hid_device.h"
#include "solar_os_native_hid_device_abi.h"

int main(void)
{
    const solar_os_native_hid_device_api_v1_t *api=solar_os_native_hid_device_get_service("hid.device",1,sizeof(*api));
    assert(api && api->abi_version==1 && api->struct_size==sizeof(*api));
    assert(!solar_os_native_hid_device_get_service("hid.device",2,8));
    assert(!solar_os_native_hid_device_get_service("hid.device",1,sizeof(*api)+1));
    assert(!solar_os_native_hid_device_get_service("hid.report",1,8));
    assert(api->init()==SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(!hid_service_checks());
    assert(!api->init());
    solar_os_hid_status_t status;
    api->get_status(&status);assert(status.initialized && status.connected);
    api->get_status(NULL);
    const uint16_t keys[]={SOLAR_OS_HID_KEY_LEFT_CTRL,4};
    assert(!api->keyboard_press(keys,2));
    assert(hid_send_keyboard() && sent_keyboard.modifier==1 && sent_keyboard.keycode[0]==4);
    assert(!api->keyboard_release(keys,2));
    assert(hid_send_keyboard() && !sent_keyboard.modifier && !sent_keyboard.keycode[0]);
    assert(api->keyboard_press(NULL,1)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->keyboard_press(keys,0)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->mouse_button(0,true)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->gamepad_button(33,true)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->gamepad_axis(SOLAR_OS_HID_AXIS_COUNT,0)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->gamepad_hat(9)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->mouse_move(17,-18) && !api->mouse_button(SOLAR_OS_HID_MOUSE_BACK,true));
    assert(hid_send_mouse() && sent_mouse.buttons==SOLAR_OS_HID_MOUSE_BACK);
    assert(!api->gamepad_axis(SOLAR_OS_HID_AXIS_RY,INT16_MAX));
    assert(!api->gamepad_button(32,true) && !api->gamepad_hat(SOLAR_OS_HID_HAT_DOWN_LEFT));
    assert(!api->gamepad_send() && hid_send_gamepad());
    assert(sent_gamepad.ry==127 && sent_gamepad.buttons==(UINT32_C(1)<<31) && sent_gamepad.hat==SOLAR_OS_HID_HAT_DOWN_LEFT);
    while(hid_send_keyboard()) {}
    for(unsigned i=0;i<HID_KEYBOARD_QUEUE_LEN;++i)assert(!api->keyboard_press(keys,2));
    assert(api->keyboard_release_all()==SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    connected=false;
    assert(api->mouse_move(0,0)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    api->release_all();assert(hid.keyboard_queue->count==1);
    connected=true;
    assert(hid_send_keyboard() && !sent_keyboard.modifier && !sent_keyboard.keycode[0]);
    assert(hid_send_mouse() && !sent_mouse.buttons);
    assert(hid_send_gamepad() && !sent_gamepad.buttons && !sent_gamepad.x && !sent_gamepad.ry && !sent_gamepad.hat);
    assert(!api->keyboard_release_all());
    hid_test_shutdown();
    puts("HID device ABI: real service, normalized errors, typed controls, queued reports and neutral cleanup passed");
    return 0;
}
