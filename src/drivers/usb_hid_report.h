#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SOLAR_OS_USB_HID_REPORT_BYTES_MAX 64U
/* Input kinds. Keyboard/mouse values match HID boot-interface protocols. */
#define SOLAR_OS_USB_HID_KEYBOARD 1U
#define SOLAR_OS_USB_HID_MOUSE 2U
#define SOLAR_OS_USB_HID_JOYSTICK 3U

typedef struct {
    uint32_t application_usage, usage, flags;
    uint16_t application, offset;
    uint8_t report_id, bits;
    int64_t logical_min, logical_max;
} solar_os_usb_hid_field_t;

typedef struct {
    bool numbered;
    uint8_t report_bytes[256];
} solar_os_usb_hid_report_info_t;

typedef esp_err_t (*solar_os_usb_hid_field_callback_t)(
    const solar_os_usb_hid_field_t *field, void *context);

/* Visits variable Input fields; offsets exclude report-ID prefixes. The
 * complete descriptor is validated before info is published. Bounded, no heap. */
esp_err_t solar_os_usb_hid_report_parse(const uint8_t *data, size_t length,
    solar_os_usb_hid_field_callback_t callback, void *context,
    solar_os_usb_hid_report_info_t *info);
esp_err_t solar_os_usb_hid_field_read(const uint8_t *payload, size_t length,
    const solar_os_usb_hid_field_t *field, int64_t *value);
