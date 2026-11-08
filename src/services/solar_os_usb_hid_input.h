#pragma once

#include "solar_os_input.h"
#include "usb_hid_mouse_report.h"

/* Boot protocol values from the USB HID specification. */
#define SOLAR_OS_USB_HID_KEYBOARD 1U
#define SOLAR_OS_USB_HID_MOUSE 2U

typedef struct {
    solar_os_input_source_t source;
    uint8_t protocol;
    uint8_t modifiers;
    uint8_t keys[6];
    uint8_t chars[6];
    uint8_t buttons;
    bool caps_lock;
    solar_os_usb_hid_mouse_layout_t mouse_layout;
} solar_os_usb_hid_input_t;

esp_err_t solar_os_usb_hid_input_open(solar_os_usb_hid_input_t *input,
                                      const char *name, uint8_t protocol);
esp_err_t solar_os_usb_hid_input_report(solar_os_usb_hid_input_t *input,
                                        const uint8_t *data, size_t length);
void solar_os_usb_hid_input_reset(solar_os_usb_hid_input_t *input);
void solar_os_usb_hid_input_close(solar_os_usb_hid_input_t *input);
