// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "solar_os_native_app_abi.h"
#include "vendor/C64Sys.h"

struct solar_os_c64_input {
    struct held_key {
        solar_os_native_foreground_key_v1_t event;
        uint8_t row, column, joystick;
        bool used, shift, translated_symbol;
    } held[48] = {};
    uint8_t modifiers = 0, keyboard_port = 0, buttons_source = 0, joystick_source = 0;
    uint8_t axes[2] = {};
    void clear();
    void key(const solar_os_native_foreground_key_v1_t &event);
    void axis(const solar_os_native_foreground_axis_v1_t &event);
    void apply(C64Sys &machine) const;
};
