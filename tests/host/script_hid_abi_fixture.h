#pragma once
#define SOLAR_OS_HID_SERVICE_FIXTURE
#include "hid_service_test.c"
#include "solar_os_native_client.h"
#include "solar_os_native_hid_device.h"
static bool hid_api_available=true;
static unsigned hid_api_lookups;
static const void *script_query_hid_device(const char *name,uint32_t abi,uint32_t minimum)
{
    ++hid_api_lookups;
    return hid_api_available?solar_os_native_hid_device_get_service(name,abi,minimum):NULL;
}
#define SOLAR_OS_SCRIPT_SERVICE_QUERY(id) script_query_##id
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
