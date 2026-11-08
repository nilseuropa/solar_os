#pragma once

#include "usb_hid_report.h"

#define SOLAR_OS_USB_HID_JOYSTICK_AXIS_MAX 6U
#define SOLAR_OS_USB_HID_JOYSTICK_BUTTON_MAX 16U
#define SOLAR_OS_USB_HID_JOYSTICK_REPORT_MAX 8U
#define SOLAR_OS_USB_HID_DIRECTION_UP 1U
#define SOLAR_OS_USB_HID_DIRECTION_DOWN 2U
#define SOLAR_OS_USB_HID_DIRECTION_LEFT 4U
#define SOLAR_OS_USB_HID_DIRECTION_RIGHT 8U

typedef struct {
    uint16_t offset;
    uint8_t bits, report_id;
} solar_os_usb_hid_joystick_field_t;

typedef struct {
    solar_os_usb_hid_joystick_field_t field;
    int64_t minimum, maximum;
} solar_os_usb_hid_joystick_range_t;

typedef struct {
    bool valid, numbered;
    uint8_t report_count;
    struct { uint8_t id, bytes; } reports[SOLAR_OS_USB_HID_JOYSTICK_REPORT_MAX];
    solar_os_usb_hid_joystick_range_t axes[SOLAR_OS_USB_HID_JOYSTICK_AXIS_MAX], hat;
    solar_os_usb_hid_joystick_field_t buttons[SOLAR_OS_USB_HID_JOYSTICK_BUTTON_MAX], dpad[4];
} solar_os_usb_hid_joystick_layout_t;

typedef struct {
    uint8_t axes_mask, directions_mask, directions;
    int16_t axes[SOLAR_OS_USB_HID_JOYSTICK_AXIS_MAX];
    uint16_t buttons_mask, buttons;
} solar_os_usb_hid_joystick_sample_t;

/* Select one Joystick/Game Pad application collection. Fields may span up to
 * eight report IDs; reports for other collections are ignored. */
esp_err_t solar_os_usb_hid_joystick_parse_descriptor(const uint8_t *data, size_t length,
    solar_os_usb_hid_joystick_layout_t *layout);
esp_err_t solar_os_usb_hid_joystick_decode(const solar_os_usb_hid_joystick_layout_t *layout,
    const uint8_t *data, size_t length, solar_os_usb_hid_joystick_sample_t *sample);
