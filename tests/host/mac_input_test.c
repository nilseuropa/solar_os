#define SOLAR_OS_MAC_PORT_IMPLEMENTATION
#include "port.h"
#include <assert.h>
#include <stdio.h>

static int moved_x, moved_y;
static bool pressed;
void mouseMove(int x, int y, int button) { moved_x = x; moved_y = y; pressed = button != 0; }
void mouseRelease(void) { moved_x = moved_y = 0; pressed = false; }
void kbdPushKey(uint8_t code, int release) { (void)code; (void)release; }
void kbdClearQueue(void) {}
void solar_os_mac_snapshot(void) {}
void solar_os_mac_report(const char *text) { (void)text; }

static void expect(int x, int y, bool button)
{
    assert(moved_x == x && moved_y == y && pressed == button);
}

int main(void)
{
    solar_os_mac_input_reset();
    solar_os_native_foreground_pointer_v1_t pointer = {
        .source = 1, .mode = SOLAR_OS_NATIVE_POINTER_RELATIVE,
        .delta_x = 9, .delta_y = -4, .buttons = 1,
    };
    solar_os_mac_pointer(&pointer); expect(45, -20, true); /* Relative input defaults to 5x. */
    assert(solar_os_mac_set_pointer_speed("1"));
    solar_os_mac_pointer(&pointer); expect(9, -4, true);
    solar_os_mac_pointer_tick(); expect(0, 0, true); /* A keyboard tick preserves a drag. */
    solar_os_native_foreground_key_v1_t key = { .usage = 0x5d }; /* Keypad 5 */
    solar_os_mac_input(&key); expect(0, 0, true);
    key.action = SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE;
    solar_os_mac_input(&key); expect(0, 0, true);
    key = (solar_os_native_foreground_key_v1_t){ .reset = true };
    solar_os_mac_input(&key); expect(0, 0, true); /* Keyboard loss keeps the device button. */

    /* Each source owns its button; another device's move/release cannot clear it. */
    pointer.source = 255; pointer.delta_x = -3; pointer.delta_y = 2;
    solar_os_mac_pointer(&pointer); expect(-3, 2, true);
    pointer.source = 1; pointer.buttons = 0;
    solar_os_mac_pointer(&pointer); expect(-3, 2, true);
    pointer.source = 255;
    solar_os_mac_pointer(&pointer); expect(-3, 2, false);
    pointer.buttons = 2; /* The Plus has one button; secondary alone does not click. */
    solar_os_mac_pointer(&pointer); expect(-3, 2, false);

    key = (solar_os_native_foreground_key_v1_t){ .usage = 0x5d };
    solar_os_mac_input(&key); expect(0, 0, true);
    pointer = (solar_os_native_foreground_pointer_v1_t){ .reset = true };
    solar_os_mac_pointer(&pointer); expect(0, 0, true); /* Pointer loss keeps keypad 5. */
    key.action = SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE;
    solar_os_mac_input(&key); expect(0, 0, false);

    assert(solar_os_mac_set_pointer_speed("5"));
    /* Touch keeps its original speed, independent of the relative multiplier. */
    pointer = (solar_os_native_foreground_pointer_v1_t){
        .source = 2, .pointer_id = 3, .mode = SOLAR_OS_NATIVE_POINTER_ABSOLUTE,
        .action = SOLAR_OS_NATIVE_POINTER_PRESS, .x = 200, .y = 100, .buttons = 1,
    };
    solar_os_mac_pointer(&pointer); expect(0, 0, true);
    pointer.action = SOLAR_OS_NATIVE_POINTER_MOVE; pointer.x = 212; pointer.y = 95;
    solar_os_mac_pointer(&pointer); expect(12, -5, true);
    pointer.action = SOLAR_OS_NATIVE_POINTER_RELEASE; pointer.buttons = 0;
    solar_os_mac_pointer(&pointer); expect(0, 0, false);
    pointer.action = SOLAR_OS_NATIVE_POINTER_PRESS; pointer.x = 20; pointer.y = 300; pointer.buttons = 1;
    solar_os_mac_pointer(&pointer); expect(0, 0, true);
    pointer.action = SOLAR_OS_NATIVE_POINTER_MOVE; pointer.source = 3;
    solar_os_mac_pointer(&pointer); expect(0, 0, true); /* No jump between sources or contacts. */
    pointer.pointer_id = 4; pointer.x = 500;
    solar_os_mac_pointer(&pointer); expect(0, 0, true);
    pointer.x = 502;
    solar_os_mac_pointer(&pointer); expect(2, 0, true);
    solar_os_mac_pointer(&(solar_os_native_foreground_pointer_v1_t){ .reset = true });
    expect(0, 0, false);
    pointer.x = 100;
    solar_os_mac_pointer(&pointer); expect(0, 0, true); /* Queue reset clears position anchors. */

    solar_os_mac_input_reset(); expect(0, 0, false);
    solar_os_mac_pointer_tick(); expect(0, 0, false);
    key = (solar_os_native_foreground_key_v1_t){ .usage = 0x5e };
    solar_os_mac_input(&key); solar_os_mac_pointer_tick(); expect(3, 0, false);
    solar_os_mac_input_reset(); solar_os_mac_pointer_tick(); expect(0, 0, false);
    pointer = (solar_os_native_foreground_pointer_v1_t){
        .mode = SOLAR_OS_NATIVE_POINTER_RELATIVE, .delta_x = 1, .delta_y = -1,
    };
    solar_os_mac_pointer(&pointer); expect(5, -5, false); /* Reset retains the setting. */
    assert(solar_os_mac_set_pointer_speed("2"));
    const char *invalid[] = {NULL, "", "0", "11", "-1", "1.5", "fast", "999999999999999999"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(!solar_os_mac_set_pointer_speed(invalid[i]));
    solar_os_mac_pointer(&pointer); expect(2, -2, false); /* Invalid values keep the previous speed. */
    assert(solar_os_mac_set_pointer_speed("10"));
    pointer.delta_x = -32768; pointer.delta_y = 32767;
    solar_os_mac_pointer(&pointer); expect(-327680, 327670, false);
    puts("Mac pointer and keyboard input: PASS");
    return 0;
}
