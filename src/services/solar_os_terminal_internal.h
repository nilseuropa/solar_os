#pragma once

#include "solar_os_terminal.h"
#include "u8g2.h"

#define SOLAR_OS_TERMINAL_SCROLLBACK_ROWS 256
#define SOLAR_OS_TERMINAL_ATTR_WORDS ((SOLAR_OS_TERMINAL_MAX_COLS + 31) / 32)
#define SOLAR_OS_TERMINAL_MAX_VRULES 8

typedef uint16_t solar_os_terminal_cell_t;

typedef struct {
    size_t row;
    size_t col;
    size_t height;
    uint8_t width;
    bool inverse;
} solar_os_terminal_vrule_t;

struct solar_os_terminal {
    u8g2_t *u8g2;
    solar_os_terminal_cell_t lines[SOLAR_OS_TERMINAL_MAX_ROWS][SOLAR_OS_TERMINAL_MAX_COLS + 1];
    uint32_t bold[SOLAR_OS_TERMINAL_MAX_ROWS][SOLAR_OS_TERMINAL_ATTR_WORDS];
    uint32_t italic[SOLAR_OS_TERMINAL_MAX_ROWS][SOLAR_OS_TERMINAL_ATTR_WORDS];
    uint32_t underline[SOLAR_OS_TERMINAL_MAX_ROWS][SOLAR_OS_TERMINAL_ATTR_WORDS];
    uint32_t inverse[SOLAR_OS_TERMINAL_MAX_ROWS][SOLAR_OS_TERMINAL_ATTR_WORDS];
    solar_os_terminal_cell_t (*scrollback)[SOLAR_OS_TERMINAL_MAX_COLS + 1];
    uint32_t (*scrollback_bold)[SOLAR_OS_TERMINAL_ATTR_WORDS];
    uint32_t (*scrollback_italic)[SOLAR_OS_TERMINAL_ATTR_WORDS];
    uint32_t (*scrollback_underline)[SOLAR_OS_TERMINAL_ATTR_WORDS];
    uint32_t (*scrollback_inverse)[SOLAR_OS_TERMINAL_ATTR_WORDS];
    size_t scrollback_capacity;
    size_t scrollback_start;
    size_t scrollback_count;
    size_t scrollback_offset;
    size_t cursor_row;
    size_t cursor_col;
    bool cursor_visible;
    size_t rows;
    size_t cols;
    solar_os_terminal_vrule_t vrules[SOLAR_OS_TERMINAL_MAX_VRULES];
    size_t vrule_count;
    uint16_t orientation_degrees;
    solar_os_terminal_font_t font;
    solar_os_terminal_text_size_t text_size;
    bool bold_enabled;
    bool italic_enabled;
    bool underline_enabled;
    bool inverse_enabled;
    bool black_is_one;
    uint8_t char_width;
    uint8_t line_height;
    uint8_t baseline_offset;
    uint8_t status_bar_height;
    solar_os_status_bar_t status_bar;
    uint32_t utf8_codepoint;
    uint8_t utf8_remaining;
    bool dirty;
};

void solar_os_terminal_init(solar_os_terminal_t *terminal, u8g2_t *u8g2);
void solar_os_terminal_inherit_text_profile(solar_os_terminal_t *terminal,
                                            const solar_os_terminal_t *source);
