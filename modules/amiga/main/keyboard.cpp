// SPDX-License-Identifier: MIT
#include "keyboard.h"
#include <string.h>

/* Amiga keyboard wire protocol: inverted, rotate-left by one, CIA-A SDR
 * interrupt; one byte at a time until the guest completes its CRA handshake. */
static uint8_t queue[256], held[128];
static unsigned first, count, delay;
static bool waiting;
static uint8_t modifiers;
static void push(uint8_t raw) { if (count < sizeof(queue)) queue[(first + count++) & 255] = raw; }
static void key(uint8_t raw, bool down)
{
    if (held[raw] == down) return;
    held[raw] = down; push(raw | (down ? 0 : 0x80));
}
void solar_os_amiga_keyboard_release(void)
{
    // Preserve any not-yet-transmitted power-up markers; discard stale input.
    bool begin = false, end = false;
    for (unsigned i = 0; i < count; ++i) {
        uint8_t raw = queue[(first + i) & 255]; begin |= raw == 0xfd; end |= raw == 0xfe;
    }
    first = count = 0;
    if (begin) push(0xfd); if (end) push(0xfe);
    for (unsigned i = 0; i < 128; ++i) if (held[i]) key(i, false);
    modifiers = 0;
}
void solar_os_amiga_keyboard_reset(void)
{
    first = count = delay = 0; waiting = false; modifiers = 0;
    memset(held, 0, sizeof(held)); push(0xfd); push(0xfe);
}
void solar_os_amiga_keyboard_ack(void) { waiting = false; delay = 360; }
void solar_os_amiga_keyboard_tick(unsigned eclocks)
{
    if (delay > eclocks) { delay -= eclocks; return; }
    delay = 0;
    if (waiting || !count) return;
    uint8_t raw = queue[first];
    uint8_t serial = (uint8_t)~((raw << 1) | (raw >> 7));
    if (!solar_os_amiga_keyboard_receive(serial)) return;
    first = (first + 1) & 255; --count; waiting = true;
}
static void set_modifiers(uint8_t mask)
{
    key(0x63, (mask & 0x11) != 0);
    key(0x60, (mask & 2) != 0); key(0x61, (mask & 0x20) != 0);
    key(0x64, (mask & 4) != 0); key(0x65, (mask & 0x40) != 0);
    key(0x66, (mask & 8) != 0); key(0x67, (mask & 0x80) != 0);
    modifiers = mask;
}
static int hid_code(unsigned usage)
{
    static const uint8_t letters[] = {0x20,0x35,0x33,0x22,0x12,0x23,0x24,0x25,0x17,0x26,0x27,0x28,0x37,0x36,0x18,0x19,0x10,0x13,0x21,0x14,0x16,0x34,0x11,0x32,0x15,0x31};
    if (usage >= 4 && usage <= 0x1d) return letters[usage - 4];
    if (usage >= 0x1e && usage <= 0x26) return usage - 0x1d;
    if (usage == 0x27) return 0x0a;
    if (usage >= 0x3a && usage <= 0x43) return 0x50 + usage - 0x3a;
    switch (usage) {
    case 0x28:return 0x44; case 0x29:return 0x45; case 0x2a:return 0x41;
    case 0x2b:return 0x42; case 0x2c:return 0x40; case 0x2d:return 0x0b;
    case 0x2e:return 0x0c; case 0x2f:return 0x1a; case 0x30:return 0x1b;
    case 0x31:return 0x0d; case 0x32:return 0x2b; case 0x33:return 0x29;
    case 0x34:return 0x2a; case 0x35:return 0; case 0x36:return 0x38;
    case 0x37:return 0x39; case 0x38:return 0x3a; case 0x39:return 0x62;
    case 0x4c:return 0x46; case 0x4f:return 0x4e; case 0x50:return 0x4f;
    case 0x51:return 0x4d; case 0x52:return 0x4c;
    case 0x54:return 0x5c; case 0x55:return 0x5d; case 0x56:return 0x4a;
    case 0x57:return 0x5e; case 0x58:return 0x43; case 0x59:return 0x1d;
    case 0x5a:return 0x1e; case 0x5b:return 0x1f; case 0x5c:return 0x2d;
    case 0x5d:return 0x2e; case 0x5e:return 0x2f; case 0x5f:return 0x3d;
    case 0x60:return 0x3e; case 0x61:return 0x3f; case 0x62:return 0x0f;
    case 0x63:return 0x3c; default:return -1;
    }
}
void solar_os_amiga_keyboard_input(const solar_os_native_foreground_key_v1_t &event)
{
    if (event.reset) { solar_os_amiga_keyboard_release(); return; }
    if (count > sizeof(queue) - 16) solar_os_amiga_keyboard_release();
    bool up = event.action == SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE;
    if (event.usage) {
        set_modifiers(event.modifiers);
        if (event.usage == 0x39) {
            if (event.action == SOLAR_OS_NATIVE_FOREGROUND_KEY_PRESS) key(0x62, !held[0x62]);
            return;
        }
        int raw = hid_code(event.usage);
        if (raw >= 0 && event.action != SOLAR_OS_NATIVE_FOREGROUND_KEY_REPEAT) key(raw, !up);
        return;
    }
    if (up) return;
    int ch = event.key, raw = -1; uint8_t mask = 0;
    if (ch == '\n' || ch == '\r') raw = 0x44;
    else if (ch == '\t') raw = 0x42;
    else if (ch == '\b' || ch == 127) raw = 0x41;
    else if (ch == 27) raw = 0x45;
    else if (ch == ' ') raw = 0x40;
    else {
        if (ch > 0 && ch < 27) { mask = 1; ch += 'a' - 1; }
        if (ch >= 'A' && ch <= 'Z') { mask |= 2; ch += 'a' - 'A'; }
        if (ch >= 'a' && ch <= 'z') raw = hid_code(ch - 'a' + 4);
        if (ch >= '1' && ch <= '9') raw = ch - '0';
        if (ch == '0') raw = 0x0a;
        static const char plain[] = "`-=\\[];'", shifted[] = "~_+|{}:\"";
        static const uint8_t codes[] = {0,0x0b,0x0c,0x0d,0x1a,0x1b,0x29,0x2a};
        for (unsigned i = 0; i < sizeof(codes); ++i) {
            if (ch == plain[i]) raw = codes[i];
            if (ch == shifted[i]) { raw = codes[i]; mask |= 2; }
        }
        static const char digits[] = "!@#$%^&*()";
        for (unsigned i = 0; i < 10; ++i) if (ch == digits[i]) { raw = i == 9 ? 0x0a : i + 1; mask |= 2; }
        const char punct[] = ",./<>?";
        for (unsigned i = 0; i < 6; ++i) if (ch == punct[i]) { raw = 0x38 + i % 3; if (i >= 3) mask |= 2; }
        if (ch == SOLAR_OS_KEY_UP) raw = 0x4c;
        if (ch == SOLAR_OS_KEY_DOWN) raw = 0x4d;
        if (ch == SOLAR_OS_KEY_LEFT) raw = 0x4f;
        if (ch == SOLAR_OS_KEY_RIGHT) raw = 0x4e;
        if (ch >= SOLAR_OS_KEY_F1 && ch <= SOLAR_OS_KEY_F10) raw = 0x50 + ch - SOLAR_OS_KEY_F1;
    }
    if (raw >= 0) {
        uint8_t saved = modifiers; set_modifiers(saved | mask);
        key(raw, true); key(raw, false); set_modifiers(saved);
    }
}
