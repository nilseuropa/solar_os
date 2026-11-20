// SPDX-License-Identifier: GPL-3.0-or-later
#include "input.h"
#include <cstring>

void solar_os_c64_input::clear()
{
    memset(held, 0, sizeof(held));
    modifiers = axes[0] = axes[1] = 0;
}

static bool position(uint8_t ch, uint8_t &row, uint8_t &column, bool &shift)
{
    // The CIA keyboard matrix, rows selected by port A and columns on port B.
    static const uint8_t matrix[8][8] = {
        {127, '\n', SOLAR_OS_KEY_RIGHT, SOLAR_OS_KEY_F7, SOLAR_OS_KEY_F1, SOLAR_OS_KEY_F3, SOLAR_OS_KEY_F5, SOLAR_OS_KEY_DOWN},
        {'3','W','A','4','Z','S','E',0}, {'5','R','D','6','C','F','T','X'},
        {'7','Y','G','8','B','H','U','V'}, {'9','I','J','0','M','K','O','N'},
        {'+','P','L','-','.',':','@',','}, {'\\','*',';',SOLAR_OS_KEY_HOME,0,'=','^','/'},
        {'1','`',0,'2',' ',0,'Q',SOLAR_OS_KEY_ESCAPE}
    };
    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    if (ch == '\r') ch = '\n';
    if (ch == 8 || ch == SOLAR_OS_KEY_DELETE) ch = 127;
    switch (ch) {
    case SOLAR_OS_KEY_UP: ch = SOLAR_OS_KEY_DOWN; shift = true; break;
    case SOLAR_OS_KEY_LEFT: ch = SOLAR_OS_KEY_RIGHT; shift = true; break;
    case SOLAR_OS_KEY_F2: ch = SOLAR_OS_KEY_F1; shift = true; break;
    case SOLAR_OS_KEY_F4: ch = SOLAR_OS_KEY_F3; shift = true; break;
    case SOLAR_OS_KEY_F6: ch = SOLAR_OS_KEY_F5; shift = true; break;
    case SOLAR_OS_KEY_F8: ch = SOLAR_OS_KEY_F7; shift = true; break;
    default: break;
    }
    static const char symbols[] = "!\"#$%&'()<>?[]";
    static const char bases[]   = "123456789,./:;";
    for (unsigned i = 0; i < sizeof(symbols) - 1; ++i)
        if (ch == (uint8_t)symbols[i]) { ch = bases[i]; shift = true; break; }
    if (!ch) return false;
    for (row = 0; row < 8; ++row)
        for (column = 0; column < 8; ++column)
            if (matrix[row][column] == ch) return true;
    return false;
}

void solar_os_c64_input::key(const solar_os_native_foreground_key_v1_t &event)
{
    if (event.reset) { clear(); return; }
    modifiers = event.modifiers;
    held_key *slot = nullptr;
    for (auto &candidate : held) {
        if (!candidate.used) { if (!slot) slot = &candidate; continue; }
        const auto &old = candidate.event;
        if (old.source == event.source && old.usage == event.usage && old.physical_key == event.physical_key &&
            (event.usage || event.physical_key || old.key == event.key)) {
            slot = &candidate; break;
        }
    }
    if (!slot) { clear(); slot = &held[0]; }
    if (event.action == SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE) {
        // An unmatched release must not remove an unrelated held key.
        if (slot->used && slot->event.source == event.source && slot->event.usage == event.usage &&
            slot->event.physical_key == event.physical_key &&
            (event.usage || event.physical_key || slot->event.key == event.key)) slot->used = false;
        return;
    }
    *slot = {};
    slot->event = event;
    bool pad = (buttons_source && event.source == buttons_source) ||
        (joystick_source && event.source == joystick_source);
    uint8_t ch = event.key;
    if (joystick_source && event.source == joystick_source && !event.usage && !ch) {
        if (event.physical_key >= 1 && event.physical_key <= 4) slot->joystick = 16;
    } else if (pad || keyboard_port) {
        switch (ch) {
        case SOLAR_OS_KEY_UP: slot->joystick = 1; break;
        case SOLAR_OS_KEY_DOWN: slot->joystick = 2; break;
        case SOLAR_OS_KEY_LEFT: slot->joystick = 4; break;
        case SOLAR_OS_KEY_RIGHT: slot->joystick = 8; break;
        case ' ': slot->joystick = 16; break;
        case '\n': case '\r': if (pad) slot->joystick = 16; break;
        default: break;
        }
    }
    if (slot->joystick) { slot->used = true; return; }
    // Modifiers normally arrive on every key event; explicit HID modifier
    // presses also work when no printable character is supplied.
    switch (event.usage) {
    case 0xe1: slot->row = 1; slot->column = 7; slot->used = true; return;
    case 0xe5: slot->row = 6; slot->column = 4; slot->used = true; return;
    case 0xe0: case 0xe4: slot->row = 7; slot->column = 2; slot->used = true; return;
    case 0xe2: case 0xe6: slot->row = 7; slot->column = 5; slot->used = true; return;
    default: break;
    }
    slot->translated_symbol = ch >= 33 && ch <= 126 &&
        !(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z');
    slot->used = position(ch, slot->row, slot->column, slot->shift);
}

void solar_os_c64_input::axis(const solar_os_native_foreground_axis_v1_t &event)
{
    if (event.reset) { axes[0] = axes[1] = 0; return; }
    if (!joystick_source || event.source != joystick_source || event.axis > 1) return;
    uint8_t negative = event.axis ? 1 : 4, positive = event.axis ? 2 : 8;
    uint8_t &previous = axes[event.axis];
    if (event.value <= -16000 || (previous == negative && event.value < -8000)) previous = negative;
    else if (event.value >= 16000 || (previous == positive && event.value > 8000)) previous = positive;
    else previous = 0;
}

void solar_os_c64_input::apply(C64Sys &machine) const
{
    memset(machine.matrix, 0xff, sizeof(machine.matrix));
    machine.joystick[0] = machine.joystick[1] = 0xff;
    uint8_t joystick = axes[0] | axes[1];
    bool shift = false, physical_shift = modifiers & 0x22, translated_symbol = false;
    for (const auto &key : held) {
        if (!key.used) continue;
        if (key.joystick) joystick |= key.joystick;
        else {
            // Logical punctuation already encodes the host layout's Shift.
            // C64 '+' and '@', for example, are unshifted guest keys.
            if ((key.row == 1 && key.column == 7) || (key.row == 6 && key.column == 4)) physical_shift = true;
            else machine.matrix[key.row] &= ~(1U << key.column);
            shift |= key.shift;
            translated_symbol |= key.translated_symbol;
        }
    }
    if (!translated_symbol) shift |= physical_shift;
    if (shift) machine.matrix[1] &= ~0x80;
    if (modifiers & 0x11) machine.matrix[7] &= ~0x04;
    if (modifiers & 0x44) machine.matrix[7] &= ~0x20;
    machine.joystick[keyboard_port == 1 ? 0 : 1] &= ~joystick;
}
