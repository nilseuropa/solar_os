#pragma once

#include "usb_hid_report.h"

typedef struct {
    bool valid, numbered;
    uint8_t report_id, report_bytes, key_count, key_max, modifiers;
    uint16_t key_offset, modifier_offsets[8];
} solar_os_usb_hid_keyboard_layout_t;

/* Select one Keyboard application with an array of up to six 8-bit usages
 * and optional modifier bits in the same input report. No heap allocation. */
esp_err_t solar_os_usb_hid_keyboard_parse_descriptor(const uint8_t *data, size_t length,
    solar_os_usb_hid_keyboard_layout_t *layout);
/* Normalize the selected report to the eight-byte boot-keyboard payload.
 * Other report IDs return ESP_ERR_NOT_FOUND without changing input state. */
esp_err_t solar_os_usb_hid_keyboard_decode(const solar_os_usb_hid_keyboard_layout_t *layout,
    const uint8_t *data, size_t length, uint8_t report[8]);
