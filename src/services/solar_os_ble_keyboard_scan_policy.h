#pragma once

#include <stdbool.h>
#include <stdint.h>

bool solar_os_ble_keyboard_scan_name_is_keyboard_like(const char *name);
bool solar_os_ble_keyboard_scan_candidate_should_replace(
    bool current_valid,
    bool current_keyboard_like,
    int8_t current_rssi,
    bool next_has_hid_service,
    bool next_keyboard_like,
    int8_t next_rssi);
