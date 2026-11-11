#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint16_t offset;
    uint8_t bits;
} solar_os_usb_hid_mouse_field_t;

typedef struct {
    bool valid;
    bool numbered;
    uint8_t report_id;
    uint8_t report_bytes;
    solar_os_usb_hid_mouse_field_t buttons[3];
    solar_os_usb_hid_mouse_field_t x, y;
} solar_os_usb_hid_mouse_layout_t;

/* Select one relative mouse report with signed 1..16-bit X/Y and buttons.
 * Offsets exclude the report-ID prefix. No descriptor memory is retained. */
esp_err_t solar_os_usb_hid_mouse_parse_descriptor(const uint8_t *data, size_t length,
    solar_os_usb_hid_mouse_layout_t *layout);

/* ESP_ERR_NOT_FOUND identifies another report ID on the same interface. */
esp_err_t solar_os_usb_hid_mouse_decode(const solar_os_usb_hid_mouse_layout_t *layout,
    const uint8_t *data, size_t length, uint8_t *buttons, int16_t *x, int16_t *y);
