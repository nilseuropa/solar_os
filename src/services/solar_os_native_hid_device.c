#include "solar_os_native_hid_device.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_hid_device_abi.h"
#include "solar_os_native_support.h"
#if SOLAR_OS_PACKAGE_SERVICE_HID
#include "solar_os_hid.h"
static int device_init(void)
{ return solar_os_native_result(solar_os_hid_init()); }
static int device_keyboard_press(const uint16_t *keys, size_t key_count)
{ return solar_os_native_result(solar_os_hid_keyboard_press(keys, key_count)); }
static int device_keyboard_release(const uint16_t *keys, size_t key_count)
{ return solar_os_native_result(solar_os_hid_keyboard_release(keys, key_count)); }
static int device_keyboard_release_all(void)
{ return solar_os_native_result(solar_os_hid_keyboard_release_all()); }
static int device_mouse_move(int32_t x, int32_t y)
{ return solar_os_native_result(solar_os_hid_mouse_move(x, y)); }
static int device_mouse_button(uint8_t button, bool pressed)
{ return solar_os_native_result(solar_os_hid_mouse_button(button, pressed)); }
static int device_gamepad_axis(solar_os_hid_axis_t axis, int16_t value)
{ return solar_os_native_result(solar_os_hid_gamepad_axis(axis, value)); }
static int device_gamepad_button(uint8_t button, bool pressed)
{ return solar_os_native_result(solar_os_hid_gamepad_button(button, pressed)); }
static int device_gamepad_hat(solar_os_hid_hat_t hat)
{ return solar_os_native_result(solar_os_hid_gamepad_hat(hat)); }
static int device_gamepad_send(void)
{ return solar_os_native_result(solar_os_hid_gamepad_send()); }
static const solar_os_native_hid_device_api_v1_t hid_device_api = {
    .abi_version = SOLAR_OS_NATIVE_HID_DEVICE_ABI, .struct_size = sizeof(hid_device_api),
    .init = device_init,
    .get_status = solar_os_hid_get_status,
    .keyboard_press = device_keyboard_press,
    .keyboard_release = device_keyboard_release,
    .keyboard_release_all = device_keyboard_release_all,
    .mouse_move = device_mouse_move,
    .mouse_button = device_mouse_button,
    .gamepad_axis = device_gamepad_axis,
    .gamepad_button = device_gamepad_button,
    .gamepad_hat = device_gamepad_hat,
    .gamepad_send = device_gamepad_send,
    .release_all = solar_os_hid_release_all,
};
#endif
const void *solar_os_native_hid_device_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_hid_device_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
