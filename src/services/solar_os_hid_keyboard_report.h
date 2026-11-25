#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "solar_os_hid_types_v1.h"

#define SOLAR_OS_HID_KEYBOARD_REPORT_STREAMS 4U

typedef struct {
    bool active;
    uint8_t map_index;
    uint16_t report_id;
    uint8_t modifiers;
    uint8_t keys[SOLAR_OS_HID_KEYBOARD_REPORT_KEYS];
} solar_os_hid_keyboard_report_stream_t;

typedef struct {
    solar_os_hid_keyboard_report_stream_t
        streams[SOLAR_OS_HID_KEYBOARD_REPORT_STREAMS];
} solar_os_hid_keyboard_report_tracker_t;

void solar_os_hid_keyboard_report_reset(
    solar_os_hid_keyboard_report_tracker_t *tracker);

/*
 * Update one HID report-protocol stream and return the union of all active
 * keyboard streams. Both the compact seven-byte report layout and the
 * eight-byte boot layout are accepted.
 */
bool solar_os_hid_keyboard_report_update(
    solar_os_hid_keyboard_report_tracker_t *tracker,
    uint8_t map_index,
    uint16_t report_id,
    const uint8_t *data,
    size_t length,
    solar_os_hid_keyboard_report_state_t *state);

/* A NULL layout retains the legacy seven/eight-byte report interpretation. */
bool solar_os_hid_keyboard_report_update_with_layout(
    solar_os_hid_keyboard_report_tracker_t *tracker,
    uint8_t map_index,
    uint16_t report_id,
    const uint8_t *data,
    size_t length,
    const solar_os_hid_keyboard_report_layout_t *layout,
    solar_os_hid_keyboard_report_state_t *state);

/* Return true when the current union adds a modifier or nonzero key usage. */
bool solar_os_hid_keyboard_report_has_new_press(
    uint8_t previous_modifiers,
    const uint8_t previous_keys[SOLAR_OS_HID_KEYBOARD_REPORT_KEYS],
    const solar_os_hid_keyboard_report_state_t *state);
