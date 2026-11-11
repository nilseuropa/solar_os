#pragma once

#include "solar_os_input.h"
#include "usb_hid_mouse_report.h"
#include "usb_hid_keyboard_report.h"
#include "usb_hid_joystick_report.h"

#define SOLAR_OS_USB_HID_HAT_PHYSICAL_BASE 0x100U

typedef struct {
    solar_os_input_source_t source;
    uint8_t protocol;
    uint8_t modifiers;
    uint8_t keys[6];
    uint8_t chars[6];
    uint8_t buttons;
    bool caps_lock;
    solar_os_usb_hid_mouse_layout_t mouse_layout;
    solar_os_usb_hid_keyboard_layout_t keyboard_layout;
    solar_os_usb_hid_joystick_layout_t joystick_layout;
    int16_t axes[SOLAR_OS_USB_HID_JOYSTICK_AXIS_MAX];
    uint16_t joystick_buttons;
    uint8_t axes_valid, directions;
} solar_os_usb_hid_input_t;

esp_err_t solar_os_usb_hid_input_open(solar_os_usb_hid_input_t *input,
                                      const char *name, uint8_t protocol);
esp_err_t solar_os_usb_hid_input_report(solar_os_usb_hid_input_t *input,
                                        const uint8_t *data, size_t length);
void solar_os_usb_hid_input_reset(solar_os_usb_hid_input_t *input);
void solar_os_usb_hid_input_close(solar_os_usb_hid_input_t *input);
