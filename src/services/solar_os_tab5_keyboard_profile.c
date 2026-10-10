#include "solar_os_tab5_keyboard.h"

#include <string.h>

/* Printed legends and matrix coordinates from M5Stack's A164 documentation
 * and Core/User/keyboard/user_keyboard_handle.c in the internal firmware.
 * Normal mode bypasses firmware character-mode latch/double-click policy.
 * Aa is held Shift; Sym selects the printed symbol layer. Literal punctuation
 * has no HID usage, because the mapper cannot encode a forced Shift per key. */
void solar_os_tab5_keyboard_map(solar_os_input_keymap_t *map)
{
    static const uint8_t usages[70] = {
        0x29,0x1e,0x1f,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0,0,0x4c,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0x2b,0x14,0x1a,0x08,0x15,0x17,0x1c,0x18,0x0c,0x12,0x13,0,0,0x2a,
        0,0xe1,0x04,0x16,0x07,0x09,0x0a,0x0b,0x0d,0x0e,0x0f,0x52,0,0x28,
        0xe0,0xe2,0x1d,0x1b,0x06,0x19,0x05,0x11,0x10,0,0x50,0x51,0x4f,0x2c,
    };
    static const char *const legends[5] = {
        "\0001234567890-+\000", "`!@#$%^&*()[]\\", "\000qwertyuiop;'\000",
        "\000\000asdfghjkl\000_\000", "\000\000zxcvbnm.\000\000\000 ",
    };
    static const char *const symbols[5] = {
        "\0001234567890-+\000", "~?@#$%^&/<>{}|", "\000qwertyuiop:\"\000",
        "\000\000asdfghjkl\000=\000", "\000\000zxcvbnm,\000\000\000 ",
    };
    memset(map, 0, sizeof(*map));
    map->rows = 5U; map->cols = 14U; map->first = 1U; map->stride = 14U;
    map->slot_count = 70U;
    for (unsigned i = 0; i < 70U; i++) {
        map->physical[i + 1U] = (uint16_t)(i + 1U);
        for (unsigned layer = 0; layer < 2U; layer++) {
            solar_os_input_keymap_key_t *key = &map->keys[layer][i + 1U];
            key->usage = usages[i];
            if (key->usage == 0U && i != 42U)
                key->key = (uint8_t)(layer ? symbols : legends)[i / 14U][i % 14U];
        }
    }
    map->keys[0][43].flags = SOLAR_OS_INPUT_KEYMAP_LAYER_TAP;
    /* Layer selectors are valid only in the base layer. */
}
