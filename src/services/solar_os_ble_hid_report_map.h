#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "solar_os_hid_keyboard_report.h"

/* Classify input report IDs belonging to Keyboard/Keypad Application
 * collections. This does not reinterpret SolarOS's existing key reports. */
bool solar_os_ble_hid_report_map(const uint8_t *data, size_t len, bool keyboard_ids[256]);

/* Extract a byte-aligned keyboard array layout for one report ID. */
bool solar_os_ble_hid_report_layout(const uint8_t *data, size_t len,
    uint8_t report_id, solar_os_hid_keyboard_report_layout_t *layout);
