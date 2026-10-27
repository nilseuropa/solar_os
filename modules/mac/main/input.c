#define SOLAR_OS_MAC_PORT_IMPLEMENTATION
#include "port.h"
#include "via.h"
#include "mouse.h"

bool solar_os_mac_paused;
static bool held[128], mouse_mode, click;
static int dx, dy;
static uint8_t modifiers;

/* USB HID usages -> Macintosh M0110 letter/punctuation scan codes. */
static const uint8_t hid_codes[0x3a] = {
    [0x04]=0x00,[0x05]=0x0b,[0x06]=0x08,[0x07]=0x02,[0x08]=0x0e,
    [0x09]=0x03,[0x0a]=0x05,[0x0b]=0x04,[0x0c]=0x22,[0x0d]=0x26,
    [0x0e]=0x28,[0x0f]=0x25,[0x10]=0x2e,[0x11]=0x2d,[0x12]=0x1f,
    [0x13]=0x23,[0x14]=0x0c,[0x15]=0x0f,[0x16]=0x01,[0x17]=0x11,
    [0x18]=0x20,[0x19]=0x09,[0x1a]=0x0d,[0x1b]=0x07,[0x1c]=0x10,
    [0x1d]=0x06,[0x1e]=0x12,[0x1f]=0x13,[0x20]=0x14,[0x21]=0x15,
    [0x22]=0x17,[0x23]=0x16,[0x24]=0x1a,[0x25]=0x1c,[0x26]=0x19,
    [0x27]=0x1d,[0x28]=0x24,[0x29]=0x35,[0x2a]=0x33,[0x2b]=0x30,
    [0x2c]=0x31,[0x2d]=0x1b,[0x2e]=0x18,[0x2f]=0x21,[0x30]=0x1e,
    [0x31]=0x2a,[0x32]=0x2a,[0x33]=0x29,[0x34]=0x27,[0x35]=0x32,[0x36]=0x2b,
    [0x37]=0x2f,[0x38]=0x2c,[0x39]=0x39,
};
static void guest_key(unsigned code, bool release)
{
    if (code >= 128 || held[code] == !release) return;
    held[code] = !release;
    kbdPushKey((uint8_t)code, release);
}
static void set_modifiers(uint8_t next)
{
    guest_key(0x38, !(next & 0x22)); /* Shift */
    guest_key(0x3a, !(next & 0x44)); /* Option */
    guest_key(0x37, !(next & 0x88)); /* Command */
    guest_key(0x3b, !(next & 0x11)); /* Control */
    modifiers = next;
}
void solar_os_mac_input_reset(void)
{
    kbdClearQueue();
    for (unsigned i = 0; i < 128; ++i) if (held[i]) guest_key(i, true);
    modifiers = 0; dx = dy = 0; click = false;
    mouseRelease();
}
void solar_os_mac_pointer_tick(void) { mouseMove(dx, dy, click); }

void solar_os_mac_input(const solar_os_native_foreground_key_v1_t *key)
{
    if (key->reset) { solar_os_mac_input_reset(); return; }
    bool release = key->action == SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE;
    set_modifiers(key->modifiers);
    if (key->usage == 0x3b || key->key == 0x87) { /* F2 */
        if (!release && key->action != SOLAR_OS_NATIVE_FOREGROUND_KEY_REPEAT) solar_os_mac_snapshot();
        return;
    }
    if (key->usage == 0x41 || key->key == 0x8d) { /* F8 */
        if (!release && key->action != SOLAR_OS_NATIVE_FOREGROUND_KEY_REPEAT) {
            solar_os_mac_input_reset(); mouse_mode = !mouse_mode;
            solar_os_mac_report(mouse_mode ? "mac: arrows move mouse; Space holds button" : "mac: keyboard mode");
        }
        return;
    }
    if (key->usage == 0x43 || key->key == 0x8f) { /* F10 */
        if (!release && key->action != SOLAR_OS_NATIVE_FOREGROUND_KEY_REPEAT) solar_os_mac_paused = !solar_os_mac_paused;
        return;
    }
    unsigned usage = key->usage;
    if (!usage && mouse_mode) {
        if (key->key >= 0x80 && key->key <= 0x83) {
            if (!release) mouseMove(key->key == 0x82 ? -6 : key->key == 0x83 ? 6 : 0,
                key->key == 0x80 ? -6 : key->key == 0x81 ? 6 : 0, click);
            return;
        }
        if (key->key == ' ') {
            if (!release) { click = !click; mouseMove(0, 0, click); }
            return;
        }
    }
    if (usage == 0x5c || (mouse_mode && (usage == 0x50 || key->key == 0x82))) { dx = release ? 0 : -3; return; }
    if (usage == 0x5e || (mouse_mode && (usage == 0x4f || key->key == 0x83))) { dx = release ? 0 : 3; return; }
    if (usage == 0x60 || (mouse_mode && (usage == 0x52 || key->key == 0x80))) { dy = release ? 0 : -3; return; }
    if (usage == 0x5a || (mouse_mode && (usage == 0x51 || key->key == 0x81))) { dy = release ? 0 : 3; return; }
    if (usage == 0x5d || (mouse_mode && (usage == 0x2c || key->key == ' '))) {
        click = !release; mouseMove(0, 0, click); return;
    }
    if (usage >= 4 && usage < sizeof(hid_codes) && (hid_codes[usage] || usage == 4)) {
        guest_key(hid_codes[usage], release); return;
    }
    /* Character-only sources have no key-up stream. Send a complete stroke.
     * HID sources use usages above and retain held modifiers. */
    if (!usage && !release && key->key && key->key < 128) {
        static const char normal[] = "abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\b\t -=[]\\#;'`,./";
        char ch = (char)key->key;
        bool shift = ch >= 'A' && ch <= 'Z';
        if (shift) ch += 'a' - 'A';
        if (ch == '\n') ch = '\r';
        if (ch == 127) ch = '\b';
        for (unsigned i = 0; i < sizeof(normal) - 1; ++i) if (normal[i] == ch) {
            uint8_t saved = modifiers;
            if (shift) set_modifiers(saved | 2U);
            guest_key(hid_codes[4 + i], false); guest_key(hid_codes[4 + i], true);
            set_modifiers(saved); break;
        }
    }
}
