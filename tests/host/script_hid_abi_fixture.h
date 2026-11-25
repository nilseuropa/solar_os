#pragma once
#define SOLAR_OS_HID_SERVICE_FIXTURE
#include "hid_service_test.c"
#include "solar_os_native_client.h"
#include "solar_os_native_hid_device.h"
static bool hid_api_available=true;
static unsigned hid_api_lookups;
static const void *hid_api_lookup(const char *name,uint32_t abi,uint32_t minimum)
{
    ++hid_api_lookups;
    return hid_api_available?solar_os_native_hid_device_get_service(name,abi,minimum):NULL;
}
static const solar_os_native_host_api_v1_t hid_host={
    .abi_version=1,.struct_size=sizeof(hid_host),.get_service=hid_api_lookup,
};
const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void) { return &hid_host; }
static void script_hid_begin(void)
{
    assert(!hid_service_checks());
    while(hid_send_keyboard()) {}
    while(hid_send_mouse()) {}
    (void)hid_send_gamepad();
}
static void script_hid_check_queued_cleanup(void)
{
    assert(hid.keyboard_queue->count==3);
    assert(hid_send_keyboard() && !sent_keyboard.modifier && !sent_keyboard.keycode[0]);
    assert(hid_send_keyboard() && sent_keyboard.keycode[0]==5);
    assert(hid_send_keyboard() && !sent_keyboard.modifier && !sent_keyboard.keycode[0]);
    assert(hid_send_mouse() && !sent_mouse.buttons);
    assert(hid_send_gamepad() && !sent_gamepad.buttons && !sent_gamepad.ry && !sent_gamepad.hat);
}
