#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "solar_os_hid_types_v1.h"

#define SOLAR_OS_HID_DESCRIPTOR_REPORT_BYTES_MAX 64U

/* Classify input report IDs belonging to Keyboard/Keypad Application
 * collections. This does not reinterpret SolarOS's existing key reports. */
bool solar_os_hid_report_descriptor_keyboard_map(const uint8_t *data, size_t len, bool keyboard_ids[256]);

/* Extract a byte-aligned keyboard array layout for one report ID. */
bool solar_os_hid_report_descriptor_keyboard_layout(const uint8_t *data, size_t len,
    uint8_t report_id, solar_os_hid_keyboard_report_layout_t *layout);
