#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { int x, y, width, height; } solar_os_screen_keyboard_rect_t;
typedef enum {
    SOLAR_OS_SCREEN_KEYBOARD_NONE, SOLAR_OS_SCREEN_KEYBOARD_EMIT,
    SOLAR_OS_SCREEN_KEYBOARD_MODIFIER, SOLAR_OS_SCREEN_KEYBOARD_CAPS,
    SOLAR_OS_SCREEN_KEYBOARD_HIDE,
    SOLAR_OS_SCREEN_KEYBOARD_REDRAW,
} solar_os_screen_keyboard_action_type_t;
typedef struct {
    solar_os_screen_keyboard_action_type_t type;
    uint8_t key, shifted, usage, modifier;
} solar_os_screen_keyboard_action_t;

typedef enum {
    SOLAR_OS_SCREEN_KEYBOARD_GLYPH_NONE,
    SOLAR_OS_SCREEN_KEYBOARD_GLYPH_SPACE,
    SOLAR_OS_SCREEN_KEYBOARD_GLYPH_ENTER,
    SOLAR_OS_SCREEN_KEYBOARD_GLYPH_BACKSPACE,
    SOLAR_OS_SCREEN_KEYBOARD_GLYPH_SHIFT,
    SOLAR_OS_SCREEN_KEYBOARD_GLYPH_TAB,
    SOLAR_OS_SCREEN_KEYBOARD_GLYPH_LEFT,
    SOLAR_OS_SCREEN_KEYBOARD_GLYPH_RIGHT,
    SOLAR_OS_SCREEN_KEYBOARD_GLYPH_UP,
    SOLAR_OS_SCREEN_KEYBOARD_GLYPH_DOWN,
} solar_os_screen_keyboard_glyph_t;

typedef struct {
    void *context;
    void (*box)(void *, solar_os_screen_keyboard_rect_t, bool filled);
    void (*text)(void *, solar_os_screen_keyboard_rect_t, const char *, bool inverse);
    void (*glyph)(void *, solar_os_screen_keyboard_rect_t, solar_os_screen_keyboard_glyph_t, bool inverse);
    void (*line)(void *, int x0, int y0, int x1, int y1);
} solar_os_screen_keyboard_painter_t;

/* Providers own geometry, drawing, hit testing, navigation, and their private
 * state. The overlay controller only handles lifecycle and emitted actions.
 * A radial or Hex-o-Spell-style provider need not expose rows or columns.
 * Return REDRAW for visual state changes without emitting a key, including
 * pointer movement through a multi-stage layout. Drawing must not mutate state;
 * the renderer may measure a layout before painting it. */
typedef struct {
    const char *name;
    size_t state_size;
    void (*init)(void *, bool symbols);
    solar_os_screen_keyboard_rect_t (*resize)(void *, int width, int height);
    void (*draw)(void *, const solar_os_screen_keyboard_painter_t *, uint8_t modifiers, bool caps);
    solar_os_screen_keyboard_action_t (*pointer)(void *, unsigned action, int x, int y);
    solar_os_screen_keyboard_action_t (*key)(void *, uint8_t key);
    void (*cancel)(void *);
} solar_os_screen_keyboard_layout_t;

const solar_os_screen_keyboard_layout_t *solar_os_screen_keyboard_layout_find(const char *name);
