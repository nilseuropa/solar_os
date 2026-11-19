#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Language keyboard layout tables, generated from keymaps/layouts/<name>.json by
 * scripts/compile_keyboard_layouts.py. See keymaps/layouts/README.md. */

enum {
    SOLAR_OS_KEYBOARD_LAYOUT_LEVEL_BASE = 0,
    SOLAR_OS_KEYBOARD_LAYOUT_LEVEL_SHIFT = 1,
    SOLAR_OS_KEYBOARD_LAYOUT_LEVEL_ALTGR = 2,
    SOLAR_OS_KEYBOARD_LAYOUT_LEVEL_SHIFT_ALTGR = 3,
    /* chars[4..7]: the same four levels while Caps Lock is on. */
    SOLAR_OS_KEYBOARD_LAYOUT_CAPS = 4,
};

typedef struct {
    uint8_t usage;     /* HID keyboard usage */
    uint8_t dead;      /* bit n: chars[n] is an index into solar_os_keyboard_dead_keys */
    uint16_t chars[8]; /* Unicode BMP code points, 0 = nothing */
} solar_os_keyboard_layout_key_t;

/* A dead key and the next character compose into one character. */
typedef struct {
    uint16_t base;
    uint16_t result;
} solar_os_keyboard_compose_t;

typedef struct {
    uint16_t spacing; /* typed on its own (dead key + space), 0 = nothing */
    uint16_t first;   /* compositions in solar_os_keyboard_compose, sorted by base */
    uint16_t count;
} solar_os_keyboard_dead_key_t;

typedef struct {
    const char *name;
    const char *description;
    bool altgr; /* some key has AltGr levels */
    uint8_t key_count;
    const solar_os_keyboard_layout_key_t *keys; /* sorted by usage */
} solar_os_keyboard_layout_t;
