#include "solar_os_terminal_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "solar_os_display.h"
#include "solar_os_fonts.h"
#include "solar_os_log.h"
#include "nvs.h"

#define TERM_MARGIN_X 4
#define TERM_MARGIN_Y 3
#define TERM_STATUS_BAR_HEIGHT 16
#define TERM_NVS_NAMESPACE "terminal"
#define TERM_NVS_ORIENTATION_KEY "orientation"
#define TERM_NVS_FONT_KEY "font"
#define TERM_NVS_TEXT_SIZE_KEY "textsize"

static const char *TAG = "solar_os_terminal";

typedef struct {
    const char *name;
} terminal_font_family_t;

typedef struct {
    const char *name;
    const uint8_t *regular_font[SOLAR_OS_TERMINAL_FONT_COMPACT + 1];
    const uint8_t *bold_font[SOLAR_OS_TERMINAL_FONT_COMPACT + 1];
    const uint8_t *italic_font[SOLAR_OS_TERMINAL_FONT_COMPACT + 1];
    const uint8_t *bold_italic_font[SOLAR_OS_TERMINAL_FONT_COMPACT + 1];
    uint8_t cell_width[SOLAR_OS_TERMINAL_FONT_COMPACT + 1];
} terminal_text_size_profile_t;

static const terminal_font_family_t terminal_font_families[] = {
    [SOLAR_OS_TERMINAL_FONT_MONO] = {
        .name = "mono",
    },
    [SOLAR_OS_TERMINAL_FONT_COMPACT] = {
        .name = "compact",
    },
};

static const terminal_text_size_profile_t terminal_text_sizes[SOLAR_OS_TERMINAL_TEXT_SIZE_COUNT] = {
    [SOLAR_OS_TERMINAL_TEXT_SIZE_12] = {
        .name = "12",
        .regular_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_r_12_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_r_12_tf,
        },
        .bold_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_b_12_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_b_12_tf,
        },
        .italic_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_i_12_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_i_12_tf,
        },
        .bold_italic_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_bi_12_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_bi_12_tf,
        },
        .cell_width = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = 6,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = 5,
        },
    },
    [SOLAR_OS_TERMINAL_TEXT_SIZE_14] = {
        .name = "14",
        .regular_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_r_14_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_r_12_tf,
        },
        .bold_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_b_14_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_b_12_tf,
        },
        .italic_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_i_14_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_i_12_tf,
        },
        .bold_italic_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_bi_14_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_bi_12_tf,
        },
        .cell_width = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = 7,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = 6,
        },
    },
    [SOLAR_OS_TERMINAL_TEXT_SIZE_16] = {
        .name = "16",
        .regular_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_r_16_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_r_14_tf,
        },
        .bold_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_b_16_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_b_14_tf,
        },
        .italic_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_i_16_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_i_14_tf,
        },
        .bold_italic_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_bi_16_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_bi_14_tf,
        },
        .cell_width = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = 8,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = 6,
        },
    },
    [SOLAR_OS_TERMINAL_TEXT_SIZE_18] = {
        .name = "18",
        .regular_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_r_18_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_r_16_tf,
        },
        .bold_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_b_18_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_b_16_tf,
        },
        .italic_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_i_18_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_i_16_tf,
        },
        .bold_italic_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_bi_18_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_bi_16_tf,
        },
        .cell_width = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = 9,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = 7,
        },
    },
    [SOLAR_OS_TERMINAL_TEXT_SIZE_20] = {
        .name = "20",
        .regular_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_r_20_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_r_18_tf,
        },
        .bold_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_b_20_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_b_18_tf,
        },
        .italic_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_i_20_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_i_18_tf,
        },
        .bold_italic_font = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = u8g2_font_solar_os_default_bi_20_tf,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = u8g2_font_solar_os_default_bi_18_tf,
        },
        .cell_width = {
            [SOLAR_OS_TERMINAL_FONT_MONO] = 10,
            [SOLAR_OS_TERMINAL_FONT_COMPACT] = 8,
        },
    },
};

static void solar_os_terminal_mark_dirty(solar_os_terminal_t *terminal);
static void terminal_return_to_live(solar_os_terminal_t *terminal);

static size_t terminal_rows(const solar_os_terminal_t *terminal)
{
    return terminal != NULL && terminal->rows > 0 ? terminal->rows : 1;
}

static size_t terminal_cols(const solar_os_terminal_t *terminal)
{
    return terminal != NULL && terminal->cols > 0 ? terminal->cols : 1;
}

static bool terminal_attr_get(const uint32_t words[SOLAR_OS_TERMINAL_ATTR_WORDS], size_t col)
{
    if (col >= SOLAR_OS_TERMINAL_MAX_COLS) {
        return false;
    }

    return (words[col / 32] & (1UL << (col % 32))) != 0;
}

static void terminal_attr_set(uint32_t words[SOLAR_OS_TERMINAL_ATTR_WORDS],
                              size_t col,
                              bool enabled)
{
    if (col >= SOLAR_OS_TERMINAL_MAX_COLS) {
        return;
    }

    const uint32_t mask = 1UL << (col % 32);
    if (enabled) {
        words[col / 32] |= mask;
    } else {
        words[col / 32] &= ~mask;
    }
}

static void terminal_attr_clear_from(uint32_t words[SOLAR_OS_TERMINAL_ATTR_WORDS], size_t col)
{
    for (size_t i = col; i < SOLAR_OS_TERMINAL_MAX_COLS; i++) {
        terminal_attr_set(words, i, false);
    }
}

static bool terminal_bold_get(const uint32_t words[SOLAR_OS_TERMINAL_ATTR_WORDS], size_t col)
{
    return terminal_attr_get(words, col);
}

static void terminal_bold_set(uint32_t words[SOLAR_OS_TERMINAL_ATTR_WORDS], size_t col, bool bold)
{
    terminal_attr_set(words, col, bold);
}

static void terminal_bold_clear_from(uint32_t words[SOLAR_OS_TERMINAL_ATTR_WORDS], size_t col)
{
    terminal_attr_clear_from(words, col);
}

static bool terminal_italic_get(const uint32_t words[SOLAR_OS_TERMINAL_ATTR_WORDS], size_t col)
{
    return terminal_attr_get(words, col);
}

static void terminal_italic_set(uint32_t words[SOLAR_OS_TERMINAL_ATTR_WORDS],
                                size_t col,
                                bool italic)
{
    terminal_attr_set(words, col, italic);
}

static void terminal_italic_clear_from(uint32_t words[SOLAR_OS_TERMINAL_ATTR_WORDS], size_t col)
{
    terminal_attr_clear_from(words, col);
}

static bool terminal_underline_get(const uint32_t words[SOLAR_OS_TERMINAL_ATTR_WORDS], size_t col)
{
    return terminal_attr_get(words, col);
}

static void terminal_underline_set(uint32_t words[SOLAR_OS_TERMINAL_ATTR_WORDS],
                                   size_t col,
                                   bool underline)
{
    terminal_attr_set(words, col, underline);
}

static void terminal_underline_clear_from(uint32_t words[SOLAR_OS_TERMINAL_ATTR_WORDS], size_t col)
{
    terminal_attr_clear_from(words, col);
}

static bool terminal_inverse_get(const uint32_t words[SOLAR_OS_TERMINAL_ATTR_WORDS], size_t col)
{
    return terminal_attr_get(words, col);
}

static void terminal_inverse_set(uint32_t words[SOLAR_OS_TERMINAL_ATTR_WORDS],
                                 size_t col,
                                 bool inverse)
{
    terminal_attr_set(words, col, inverse);
}

static void terminal_inverse_clear_from(uint32_t words[SOLAR_OS_TERMINAL_ATTR_WORDS], size_t col)
{
    terminal_attr_clear_from(words, col);
}

static const uint8_t *terminal_selected_font(const solar_os_terminal_t *terminal,
                                             bool bold,
                                             bool italic)
{
    solar_os_terminal_font_t font = SOLAR_OS_TERMINAL_FONT_MONO;
    solar_os_terminal_text_size_t text_size = SOLAR_OS_TERMINAL_TEXT_SIZE_14;

    if (terminal != NULL) {
        font = terminal->font;
        text_size = terminal->text_size;
    }

    if ((size_t)font >= sizeof(terminal_font_families) / sizeof(terminal_font_families[0])) {
        font = SOLAR_OS_TERMINAL_FONT_MONO;
    }
    if ((size_t)text_size >= sizeof(terminal_text_sizes) / sizeof(terminal_text_sizes[0]) ||
        terminal_text_sizes[text_size].name == NULL) {
        text_size = SOLAR_OS_TERMINAL_TEXT_SIZE_14;
    }

    const terminal_text_size_profile_t *profile = &terminal_text_sizes[text_size];
    if (bold && italic && profile->bold_italic_font[font] != NULL) {
        return profile->bold_italic_font[font];
    }
    if (italic && profile->italic_font[font] != NULL) {
        return profile->italic_font[font];
    }
    if (bold && profile->bold_font[font] != NULL) {
        return profile->bold_font[font];
    }
    if (profile->regular_font[font] != NULL) {
        return profile->regular_font[font];
    }
    return u8g2_font_solar_os_default_r_14_tf;
}

static uint8_t terminal_text_scale(const solar_os_terminal_t *terminal)
{
    (void)terminal;
    return 1;
}

static bool terminal_is_printable(char ch)
{
    const unsigned char value = (unsigned char)ch;

    return isprint(value) || value >= 0xa0;
}

static bool terminal_is_combining_codepoint(uint32_t codepoint)
{
    return (codepoint >= 0x0300 && codepoint <= 0x036f) ||
        (codepoint >= 0x1ab0 && codepoint <= 0x1aff) ||
        (codepoint >= 0x1dc0 && codepoint <= 0x1dff) ||
        (codepoint >= 0x20d0 && codepoint <= 0x20ff) ||
        (codepoint >= 0xfe20 && codepoint <= 0xfe2f);
}

static bool terminal_is_printable_codepoint(uint32_t codepoint)
{
    return (codepoint >= 0x20 && codepoint <= 0x7e) ||
        (codepoint >= 0xa0 && codepoint <= 0xd7ff) ||
        (codepoint >= 0xe000 && codepoint <= 0xffff);
}

static solar_os_terminal_cell_t terminal_cell_for_codepoint(uint32_t codepoint)
{
    if (terminal_is_combining_codepoint(codepoint)) {
        return 0;
    }
    if (codepoint > 0xffff) {
        return '?';
    }
    if (!terminal_is_printable_codepoint(codepoint)) {
        return 0;
    }
    return (solar_os_terminal_cell_t)codepoint;
}

static size_t terminal_line_len(const solar_os_terminal_t *terminal,
                                const solar_os_terminal_cell_t *line)
{
    const size_t cols = terminal_cols(terminal);

    if (line == NULL) {
        return 0;
    }

    size_t len = 0;
    while (len < cols && line[len] != 0) {
        len++;
    }
    return len;
}

static const u8g2_cb_t *terminal_rotation_cb(uint16_t degrees)
{
    switch (degrees) {
    case 0:
        return U8G2_R1;
    case 90:
        return U8G2_R2;
    case 180:
        return U8G2_R3;
    case 270:
        return U8G2_R0;
    default:
        return NULL;
    }
}

static bool terminal_orientation_is_valid(uint16_t degrees)
{
    return terminal_rotation_cb(degrees) != NULL;
}

static bool terminal_text_size_is_valid(solar_os_terminal_text_size_t text_size)
{
    return (size_t)text_size < sizeof(terminal_text_sizes) / sizeof(terminal_text_sizes[0]) &&
        terminal_text_sizes[text_size].name != NULL;
}

static esp_err_t terminal_save_u16(const char *key, uint16_t value)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(TERM_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u16(nvs, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static void terminal_load_settings(solar_os_terminal_t *terminal)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(TERM_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        return;
    }

    uint16_t value = 0;
    ret = nvs_get_u16(nvs, TERM_NVS_ORIENTATION_KEY, &value);
    if (ret == ESP_OK && terminal_orientation_is_valid(value)) {
        terminal->orientation_degrees = value;
    }

    ret = nvs_get_u16(nvs, TERM_NVS_FONT_KEY, &value);
    if (ret == ESP_OK &&
        value < sizeof(terminal_font_families) / sizeof(terminal_font_families[0])) {
        terminal->font = (solar_os_terminal_font_t)value;
    }

    ret = nvs_get_u16(nvs, TERM_NVS_TEXT_SIZE_KEY, &value);
    if (ret == ESP_OK &&
        value < sizeof(terminal_text_sizes) / sizeof(terminal_text_sizes[0]) &&
        terminal_text_sizes[value].name != NULL) {
        terminal->text_size = (solar_os_terminal_text_size_t)value;
    }

    nvs_close(nvs);
}

static void terminal_apply_settings(solar_os_terminal_t *terminal, bool clear_screen)
{
    if (terminal == NULL || terminal->u8g2 == NULL) {
        return;
    }

    u8g2_t *u8g2 = terminal->u8g2;
    const u8g2_cb_t *rotation = terminal_rotation_cb(terminal->orientation_degrees);
    if (rotation != NULL) {
        u8g2_SetDisplayRotation(u8g2, rotation);
    }

    u8g2_SetFont(u8g2, terminal_selected_font(terminal, false, false));
    u8g2_SetFontMode(u8g2, 1);
    u8g2_SetFontPosBaseline(u8g2);

    const uint8_t text_scale = terminal_text_scale(terminal);
    int regular_width = u8g2_GetStrWidth(u8g2, "M");
    if (regular_width <= 0) {
        regular_width = u8g2_GetMaxCharWidth(u8g2);
    }
    u8g2_SetFont(u8g2, terminal_selected_font(terminal, true, false));
    int bold_width = u8g2_GetStrWidth(u8g2, "M");
    if (bold_width <= 0) {
        bold_width = u8g2_GetMaxCharWidth(u8g2);
    }
    u8g2_SetFont(u8g2, terminal_selected_font(terminal, false, false));

    int char_width = regular_width > bold_width ? regular_width : bold_width;
    const terminal_text_size_profile_t *profile =
        terminal_text_size_is_valid(terminal->text_size) ?
        &terminal_text_sizes[terminal->text_size] :
        &terminal_text_sizes[SOLAR_OS_TERMINAL_TEXT_SIZE_14];
    if ((size_t)terminal->font < sizeof(profile->cell_width) / sizeof(profile->cell_width[0]) &&
        profile->cell_width[terminal->font] > 0) {
        char_width = profile->cell_width[terminal->font];
    }
    if (char_width <= 0) {
        char_width = 6;
    }
    char_width *= text_scale;

    int regular_height = u8g2_GetMaxCharHeight(u8g2);
    if (regular_height <= 0) {
        regular_height = u8g2_GetAscent(u8g2) - u8g2_GetDescent(u8g2);
    }

    u8g2_SetFont(u8g2, terminal_selected_font(terminal, true, false));
    int bold_height = u8g2_GetMaxCharHeight(u8g2);
    if (bold_height <= 0) {
        bold_height = u8g2_GetAscent(u8g2) - u8g2_GetDescent(u8g2);
    }

    u8g2_SetFont(u8g2, terminal_selected_font(terminal, false, false));
    int line_height = (regular_height > bold_height ? regular_height : bold_height) + 1;
    if (line_height <= 1) {
        line_height = 14;
    }
    line_height *= text_scale;

    const int display_width = u8g2_GetDisplayWidth(u8g2);
    const int display_height = u8g2_GetDisplayHeight(u8g2);
    int status_bar_height = TERM_STATUS_BAR_HEIGHT;
    if (status_bar_height > display_height / 3) {
        status_bar_height = display_height / 3;
    }

    int baseline_offset =
        status_bar_height + TERM_MARGIN_Y + (u8g2_GetAscent(u8g2) * text_scale);
    if (baseline_offset <= status_bar_height + TERM_MARGIN_Y) {
        baseline_offset = status_bar_height + TERM_MARGIN_Y + line_height - 1;
    }

    int text_bottom = display_height - TERM_MARGIN_Y;
    if (text_bottom < baseline_offset) {
        text_bottom = baseline_offset;
    }

    size_t cols = (size_t)((display_width - (TERM_MARGIN_X * 2)) / char_width);
    if (cols < 1) {
        cols = 1;
    } else if (cols > SOLAR_OS_TERMINAL_MAX_COLS) {
        cols = SOLAR_OS_TERMINAL_MAX_COLS;
    }

    size_t rows = (size_t)((text_bottom - baseline_offset) / line_height) + 1;
    if (rows < 1) {
        rows = 1;
    } else if (rows > SOLAR_OS_TERMINAL_MAX_ROWS) {
        rows = SOLAR_OS_TERMINAL_MAX_ROWS;
    }

    terminal->cols = cols;
    terminal->rows = rows;
    terminal->char_width = (uint8_t)char_width;
    terminal->line_height = (uint8_t)line_height;
    terminal->baseline_offset = (uint8_t)baseline_offset;
    terminal->status_bar_height = (uint8_t)status_bar_height;

    if (clear_screen) {
        solar_os_terminal_clear(terminal);
    } else {
        for (size_t row = 0; row < SOLAR_OS_TERMINAL_MAX_ROWS; row++) {
            terminal->lines[row][cols] = '\0';
            terminal_bold_clear_from(terminal->bold[row], cols);
            terminal_italic_clear_from(terminal->italic[row], cols);
            terminal_underline_clear_from(terminal->underline[row], cols);
            terminal_inverse_clear_from(terminal->inverse[row], cols);
        }
        for (size_t row = 0; row < terminal->scrollback_capacity; row++) {
            if (terminal->scrollback != NULL) {
                terminal->scrollback[row][cols] = '\0';
            }
            if (terminal->scrollback_bold != NULL) {
                terminal_bold_clear_from(terminal->scrollback_bold[row], cols);
            }
            if (terminal->scrollback_italic != NULL) {
                terminal_italic_clear_from(terminal->scrollback_italic[row], cols);
            }
            if (terminal->scrollback_underline != NULL) {
                terminal_underline_clear_from(terminal->scrollback_underline[row], cols);
            }
            if (terminal->scrollback_inverse != NULL) {
                terminal_inverse_clear_from(terminal->scrollback_inverse[row], cols);
            }
        }
        if (terminal->scrollback_offset > terminal->scrollback_count) {
            terminal->scrollback_offset = terminal->scrollback_count;
        }
        if (terminal->cursor_row >= rows) {
            terminal->cursor_row = rows - 1;
        }
        if (terminal->cursor_col > cols) {
            terminal->cursor_col = cols;
        }
        solar_os_terminal_mark_dirty(terminal);
    }
}

static void solar_os_terminal_mark_dirty(solar_os_terminal_t *terminal)
{
    if (terminal != NULL) {
        terminal->dirty = true;
    }
}

static void terminal_return_to_live(solar_os_terminal_t *terminal)
{
    if (terminal != NULL && terminal->scrollback_offset != 0) {
        terminal->scrollback_offset = 0;
        solar_os_terminal_mark_dirty(terminal);
    }
}

static const solar_os_terminal_cell_t *scrollback_line(const solar_os_terminal_t *terminal,
                                                       size_t index)
{
    if (terminal == NULL || terminal->scrollback == NULL || terminal->scrollback_capacity == 0 ||
        index >= terminal->scrollback_count) {
        return NULL;
    }

    const size_t slot = (terminal->scrollback_start + index) % terminal->scrollback_capacity;
    return terminal->scrollback[slot];
}

static const uint32_t *scrollback_bold_line(const solar_os_terminal_t *terminal, size_t index)
{
    static const uint32_t no_bold[SOLAR_OS_TERMINAL_ATTR_WORDS];

    if (terminal == NULL || terminal->scrollback_bold == NULL ||
        terminal->scrollback_capacity == 0 || index >= terminal->scrollback_count) {
        return no_bold;
    }

    const size_t slot = (terminal->scrollback_start + index) % terminal->scrollback_capacity;
    return terminal->scrollback_bold[slot];
}

static const uint32_t *scrollback_italic_line(const solar_os_terminal_t *terminal, size_t index)
{
    static const uint32_t no_italic[SOLAR_OS_TERMINAL_ATTR_WORDS];

    if (terminal == NULL || terminal->scrollback_italic == NULL ||
        terminal->scrollback_capacity == 0 || index >= terminal->scrollback_count) {
        return no_italic;
    }

    const size_t slot = (terminal->scrollback_start + index) % terminal->scrollback_capacity;
    return terminal->scrollback_italic[slot];
}

static const uint32_t *scrollback_underline_line(const solar_os_terminal_t *terminal, size_t index)
{
    static const uint32_t no_underline[SOLAR_OS_TERMINAL_ATTR_WORDS];

    if (terminal == NULL || terminal->scrollback_underline == NULL ||
        terminal->scrollback_capacity == 0 || index >= terminal->scrollback_count) {
        return no_underline;
    }

    const size_t slot = (terminal->scrollback_start + index) % terminal->scrollback_capacity;
    return terminal->scrollback_underline[slot];
}

static const uint32_t *scrollback_inverse_line(const solar_os_terminal_t *terminal, size_t index)
{
    static const uint32_t no_inverse[SOLAR_OS_TERMINAL_ATTR_WORDS];

    if (terminal == NULL || terminal->scrollback_inverse == NULL ||
        terminal->scrollback_capacity == 0 || index >= terminal->scrollback_count) {
        return no_inverse;
    }

    const size_t slot = (terminal->scrollback_start + index) % terminal->scrollback_capacity;
    return terminal->scrollback_inverse[slot];
}

static void scrollback_push(solar_os_terminal_t *terminal,
                            const solar_os_terminal_cell_t *line,
                            const uint32_t bold[SOLAR_OS_TERMINAL_ATTR_WORDS],
                            const uint32_t italic[SOLAR_OS_TERMINAL_ATTR_WORDS],
                            const uint32_t underline[SOLAR_OS_TERMINAL_ATTR_WORDS],
                            const uint32_t inverse[SOLAR_OS_TERMINAL_ATTR_WORDS])
{
    if (terminal == NULL ||
        terminal->scrollback == NULL ||
        terminal->scrollback_bold == NULL ||
        terminal->scrollback_italic == NULL ||
        terminal->scrollback_underline == NULL ||
        terminal->scrollback_inverse == NULL ||
        terminal->scrollback_capacity == 0) {
        return;
    }

    const size_t capacity = terminal->scrollback_capacity;
    size_t slot = 0;
    if (terminal->scrollback_count < capacity) {
        slot = (terminal->scrollback_start + terminal->scrollback_count) % capacity;
        terminal->scrollback_count++;
    } else {
        slot = terminal->scrollback_start;
        terminal->scrollback_start = (terminal->scrollback_start + 1) % capacity;
    }

    if (line != NULL) {
        memcpy(terminal->scrollback[slot],
               line,
               sizeof(terminal->scrollback[slot]));
    } else {
        memset(terminal->scrollback[slot], 0, sizeof(terminal->scrollback[slot]));
    }
    terminal->scrollback[slot][terminal_cols(terminal)] = 0;
    if (bold != NULL) {
        memcpy(terminal->scrollback_bold[slot],
               bold,
               sizeof(terminal->scrollback_bold[slot]));
    } else {
        memset(terminal->scrollback_bold[slot], 0, sizeof(terminal->scrollback_bold[slot]));
    }
    if (italic != NULL) {
        memcpy(terminal->scrollback_italic[slot],
               italic,
               sizeof(terminal->scrollback_italic[slot]));
    } else {
        memset(terminal->scrollback_italic[slot], 0, sizeof(terminal->scrollback_italic[slot]));
    }
    if (underline != NULL) {
        memcpy(terminal->scrollback_underline[slot],
               underline,
               sizeof(terminal->scrollback_underline[slot]));
    } else {
        memset(terminal->scrollback_underline[slot],
               0,
               sizeof(terminal->scrollback_underline[slot]));
    }
    if (inverse != NULL) {
        memcpy(terminal->scrollback_inverse[slot],
               inverse,
               sizeof(terminal->scrollback_inverse[slot]));
    } else {
        memset(terminal->scrollback_inverse[slot], 0, sizeof(terminal->scrollback_inverse[slot]));
    }
}

static void scroll_up(solar_os_terminal_t *terminal)
{
    const size_t rows = terminal_rows(terminal);

    scrollback_push(terminal,
                    terminal->lines[0],
                    terminal->bold[0],
                    terminal->italic[0],
                    terminal->underline[0],
                    terminal->inverse[0]);
    memmove(terminal->lines[0],
            terminal->lines[1],
            sizeof(terminal->lines[0]) * (rows - 1));
    memmove(terminal->bold[0],
            terminal->bold[1],
            sizeof(terminal->bold[0]) * (rows - 1));
    memmove(terminal->italic[0],
            terminal->italic[1],
            sizeof(terminal->italic[0]) * (rows - 1));
    memmove(terminal->underline[0],
            terminal->underline[1],
            sizeof(terminal->underline[0]) * (rows - 1));
    memmove(terminal->inverse[0],
            terminal->inverse[1],
            sizeof(terminal->inverse[0]) * (rows - 1));
    memset(terminal->lines[rows - 1],
           0,
           sizeof(terminal->lines[rows - 1]));
    memset(terminal->bold[rows - 1], 0, sizeof(terminal->bold[rows - 1]));
    memset(terminal->italic[rows - 1], 0, sizeof(terminal->italic[rows - 1]));
    memset(terminal->underline[rows - 1], 0, sizeof(terminal->underline[rows - 1]));
    memset(terminal->inverse[rows - 1], 0, sizeof(terminal->inverse[rows - 1]));
    terminal->cursor_row = rows - 1;
    terminal->cursor_col = 0;
}

static void terminal_alloc_scrollback(solar_os_terminal_t *terminal)
{
    if (terminal == NULL) {
        return;
    }

    terminal->scrollback = heap_caps_calloc(SOLAR_OS_TERMINAL_SCROLLBACK_ROWS,
                                            sizeof(terminal->scrollback[0]),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    terminal->scrollback_bold = heap_caps_calloc(SOLAR_OS_TERMINAL_SCROLLBACK_ROWS,
                                                 sizeof(terminal->scrollback_bold[0]),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    terminal->scrollback_italic = heap_caps_calloc(SOLAR_OS_TERMINAL_SCROLLBACK_ROWS,
                                                   sizeof(terminal->scrollback_italic[0]),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    terminal->scrollback_underline = heap_caps_calloc(SOLAR_OS_TERMINAL_SCROLLBACK_ROWS,
                                                      sizeof(terminal->scrollback_underline[0]),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    terminal->scrollback_inverse = heap_caps_calloc(SOLAR_OS_TERMINAL_SCROLLBACK_ROWS,
                                                    sizeof(terminal->scrollback_inverse[0]),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (terminal->scrollback == NULL ||
        terminal->scrollback_bold == NULL ||
        terminal->scrollback_italic == NULL ||
        terminal->scrollback_underline == NULL ||
        terminal->scrollback_inverse == NULL) {
        heap_caps_free(terminal->scrollback);
        heap_caps_free(terminal->scrollback_bold);
        heap_caps_free(terminal->scrollback_italic);
        heap_caps_free(terminal->scrollback_underline);
        heap_caps_free(terminal->scrollback_inverse);
        terminal->scrollback = NULL;
        terminal->scrollback_bold = NULL;
        terminal->scrollback_italic = NULL;
        terminal->scrollback_underline = NULL;
        terminal->scrollback_inverse = NULL;
        terminal->scrollback_capacity = 0;
        SOLAR_OS_LOGW(TAG, "scrollback disabled: PSRAM allocation failed");
        return;
    }

    terminal->scrollback_capacity = SOLAR_OS_TERMINAL_SCROLLBACK_ROWS;
    SOLAR_OS_LOGI(TAG, "scrollback ready: %u lines in PSRAM", (unsigned)terminal->scrollback_capacity);
}

void solar_os_terminal_init(solar_os_terminal_t *terminal, u8g2_t *u8g2)
{
    if (terminal == NULL) {
        return;
    }

    memset(terminal, 0, sizeof(*terminal));
    terminal_alloc_scrollback(terminal);
    terminal->u8g2 = u8g2;
    terminal->orientation_degrees = 0;
    terminal->font = SOLAR_OS_TERMINAL_FONT_MONO;
    terminal->text_size = SOLAR_OS_TERMINAL_TEXT_SIZE_14;
    terminal->cols = 65;
    terminal->rows = 20;
    terminal->char_width = 7;
    terminal->line_height = 14;
    terminal->baseline_offset = 13;
    terminal->status_bar_height = TERM_STATUS_BAR_HEIGHT;
    terminal->cursor_visible = true;
    terminal_load_settings(terminal);
    terminal_apply_settings(terminal, false);
    terminal->dirty = true;
}

void solar_os_terminal_set_black_is_one(solar_os_terminal_t *terminal, bool black_is_one)
{
    if (terminal == NULL) {
        return;
    }

    terminal->black_is_one = black_is_one;
    terminal->dirty = true;
}

void solar_os_terminal_clear(solar_os_terminal_t *terminal)
{
    if (terminal == NULL) {
        return;
    }

    memset(terminal->lines, 0, sizeof(terminal->lines));
    memset(terminal->bold, 0, sizeof(terminal->bold));
    memset(terminal->italic, 0, sizeof(terminal->italic));
    memset(terminal->underline, 0, sizeof(terminal->underline));
    memset(terminal->inverse, 0, sizeof(terminal->inverse));
    if (terminal->scrollback != NULL && terminal->scrollback_capacity > 0) {
        memset(terminal->scrollback,
               0,
               terminal->scrollback_capacity * sizeof(terminal->scrollback[0]));
    }
    if (terminal->scrollback_bold != NULL && terminal->scrollback_capacity > 0) {
        memset(terminal->scrollback_bold,
               0,
               terminal->scrollback_capacity * sizeof(terminal->scrollback_bold[0]));
    }
    if (terminal->scrollback_italic != NULL && terminal->scrollback_capacity > 0) {
        memset(terminal->scrollback_italic,
               0,
               terminal->scrollback_capacity * sizeof(terminal->scrollback_italic[0]));
    }
    if (terminal->scrollback_underline != NULL && terminal->scrollback_capacity > 0) {
        memset(terminal->scrollback_underline,
               0,
               terminal->scrollback_capacity * sizeof(terminal->scrollback_underline[0]));
    }
    if (terminal->scrollback_inverse != NULL && terminal->scrollback_capacity > 0) {
        memset(terminal->scrollback_inverse,
               0,
               terminal->scrollback_capacity * sizeof(terminal->scrollback_inverse[0]));
    }
    terminal->scrollback_start = 0;
    terminal->scrollback_count = 0;
    terminal->scrollback_offset = 0;
    terminal->cursor_row = 0;
    terminal->cursor_col = 0;
    terminal->cursor_visible = true;
    terminal->vrule_count = 0;
    solar_os_terminal_utf8_reset(terminal);
    solar_os_terminal_mark_dirty(terminal);
}

void solar_os_terminal_newline(solar_os_terminal_t *terminal)
{
    if (terminal == NULL) {
        return;
    }

    terminal_return_to_live(terminal);
    terminal->cursor_col = 0;
    if (terminal->cursor_row + 1 >= terminal_rows(terminal)) {
        scroll_up(terminal);
    } else {
        terminal->cursor_row++;
    }

    solar_os_terminal_mark_dirty(terminal);
}

void solar_os_terminal_backspace(solar_os_terminal_t *terminal)
{
    if (terminal == NULL) {
        return;
    }

    terminal_return_to_live(terminal);
    if (terminal->cursor_col > 0) {
        terminal->cursor_col--;
        terminal->lines[terminal->cursor_row][terminal->cursor_col] = 0;
        terminal_bold_set(terminal->bold[terminal->cursor_row], terminal->cursor_col, false);
        terminal_italic_set(terminal->italic[terminal->cursor_row], terminal->cursor_col, false);
        terminal_underline_set(terminal->underline[terminal->cursor_row],
                               terminal->cursor_col,
                               false);
        terminal_inverse_set(terminal->inverse[terminal->cursor_row], terminal->cursor_col, false);
        solar_os_terminal_mark_dirty(terminal);
        return;
    }

    if (terminal->cursor_row == 0) {
        return;
    }

    terminal->cursor_row--;
    terminal->cursor_col = terminal_line_len(terminal, terminal->lines[terminal->cursor_row]);
    if (terminal->cursor_col > 0) {
        terminal->cursor_col--;
        terminal->lines[terminal->cursor_row][terminal->cursor_col] = 0;
        terminal_bold_set(terminal->bold[terminal->cursor_row], terminal->cursor_col, false);
        terminal_italic_set(terminal->italic[terminal->cursor_row], terminal->cursor_col, false);
        terminal_underline_set(terminal->underline[terminal->cursor_row],
                               terminal->cursor_col,
                               false);
        terminal_inverse_set(terminal->inverse[terminal->cursor_row], terminal->cursor_col, false);
        solar_os_terminal_mark_dirty(terminal);
    }
}

void solar_os_terminal_put_codepoint(solar_os_terminal_t *terminal, uint32_t codepoint)
{
    if (terminal == NULL) {
        return;
    }
    const solar_os_terminal_cell_t cell = terminal_cell_for_codepoint(codepoint);
    if (cell == 0) {
        return;
    }

    terminal_return_to_live(terminal);
    if (terminal->cursor_col >= terminal_cols(terminal)) {
        solar_os_terminal_newline(terminal);
    }

    solar_os_terminal_cell_t *line = terminal->lines[terminal->cursor_row];
    const size_t line_len = terminal_line_len(terminal, line);
    for (size_t col = line_len; col < terminal->cursor_col; col++) {
        line[col] = ' ';
        terminal_bold_set(terminal->bold[terminal->cursor_row], col, false);
        terminal_italic_set(terminal->italic[terminal->cursor_row], col, false);
        terminal_underline_set(terminal->underline[terminal->cursor_row], col, false);
        terminal_inverse_set(terminal->inverse[terminal->cursor_row], col, false);
    }

    terminal->lines[terminal->cursor_row][terminal->cursor_col] = cell;
    terminal_bold_set(terminal->bold[terminal->cursor_row],
                      terminal->cursor_col,
                      terminal->bold_enabled);
    terminal_italic_set(terminal->italic[terminal->cursor_row],
                        terminal->cursor_col,
                        terminal->italic_enabled);
    terminal_underline_set(terminal->underline[terminal->cursor_row],
                           terminal->cursor_col,
                           terminal->underline_enabled);
    terminal_inverse_set(terminal->inverse[terminal->cursor_row],
                         terminal->cursor_col,
                         terminal->inverse_enabled);
    terminal->cursor_col++;
    if (terminal->cursor_col > line_len) {
        terminal->lines[terminal->cursor_row][terminal->cursor_col] = 0;
    }
    solar_os_terminal_mark_dirty(terminal);
}

void solar_os_terminal_put_printable(solar_os_terminal_t *terminal, char ch)
{
    solar_os_terminal_put_codepoint(terminal, (unsigned char)ch);
}

void solar_os_terminal_put_char(solar_os_terminal_t *terminal, char ch)
{
    if (terminal == NULL) {
        return;
    }

    switch (ch) {
    case '\r':
        terminal_return_to_live(terminal);
        terminal->cursor_col = 0;
        solar_os_terminal_mark_dirty(terminal);
        break;
    case '\n':
        solar_os_terminal_newline(terminal);
        break;
    case '\b':
        solar_os_terminal_backspace(terminal);
        break;
    case '\t':
        do {
            solar_os_terminal_put_printable(terminal, ' ');
        } while ((terminal->cursor_col % 4) != 0);
        break;
    default:
        if (terminal_is_printable(ch)) {
            solar_os_terminal_put_printable(terminal, ch);
        }
        break;
    }
}

void solar_os_terminal_utf8_reset(solar_os_terminal_t *terminal)
{
    if (terminal == NULL) {
        return;
    }

    terminal->utf8_codepoint = 0;
    terminal->utf8_remaining = 0;
}

static void terminal_utf8_emit_replacement(solar_os_terminal_t *terminal)
{
    solar_os_terminal_put_codepoint(terminal, '?');
}

void solar_os_terminal_put_utf8_byte(solar_os_terminal_t *terminal, uint8_t byte)
{
    if (terminal == NULL) {
        return;
    }

    if (terminal->utf8_remaining == 0) {
        if ((byte & 0x80) == 0) {
            solar_os_terminal_put_char(terminal, (char)byte);
        } else if ((byte & 0xe0) == 0xc0) {
            terminal->utf8_codepoint = byte & 0x1f;
            terminal->utf8_remaining = 1;
        } else if ((byte & 0xf0) == 0xe0) {
            terminal->utf8_codepoint = byte & 0x0f;
            terminal->utf8_remaining = 2;
        } else if ((byte & 0xf8) == 0xf0) {
            terminal->utf8_codepoint = byte & 0x07;
            terminal->utf8_remaining = 3;
        } else {
            terminal_utf8_emit_replacement(terminal);
        }
        return;
    }

    if ((byte & 0xc0) != 0x80) {
        solar_os_terminal_utf8_reset(terminal);
        terminal_utf8_emit_replacement(terminal);
        solar_os_terminal_put_utf8_byte(terminal, byte);
        return;
    }

    terminal->utf8_codepoint = (terminal->utf8_codepoint << 6) | (byte & 0x3f);
    terminal->utf8_remaining--;
    if (terminal->utf8_remaining == 0) {
        solar_os_terminal_put_codepoint(terminal, terminal->utf8_codepoint);
        terminal->utf8_codepoint = 0;
    }
}

void solar_os_terminal_write_utf8(solar_os_terminal_t *terminal, const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; p != NULL && *p != '\0'; p++) {
        solar_os_terminal_put_utf8_byte(terminal, *p);
    }
}

void solar_os_terminal_write(solar_os_terminal_t *terminal, const char *text)
{
    for (const char *p = text; p != NULL && *p != '\0'; p++) {
        solar_os_terminal_put_char(terminal, *p);
    }
}

void solar_os_terminal_writeln(solar_os_terminal_t *terminal, const char *text)
{
    solar_os_terminal_write(terminal, text);
    solar_os_terminal_put_char(terminal, '\n');
}

void solar_os_terminal_printf(solar_os_terminal_t *terminal, const char *fmt, ...)
{
    char buffer[160];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    solar_os_terminal_write(terminal, buffer);
}

void solar_os_terminal_set_bold(solar_os_terminal_t *terminal, bool enabled)
{
    if (terminal == NULL) {
        return;
    }

    terminal->bold_enabled = enabled;
}

bool solar_os_terminal_bold(const solar_os_terminal_t *terminal)
{
    return terminal != NULL && terminal->bold_enabled;
}

void solar_os_terminal_set_italic(solar_os_terminal_t *terminal, bool enabled)
{
    if (terminal == NULL) {
        return;
    }

    terminal->italic_enabled = enabled;
}

bool solar_os_terminal_italic(const solar_os_terminal_t *terminal)
{
    return terminal != NULL && terminal->italic_enabled;
}

void solar_os_terminal_set_underline(solar_os_terminal_t *terminal, bool enabled)
{
    if (terminal == NULL) {
        return;
    }

    terminal->underline_enabled = enabled;
}

bool solar_os_terminal_underline(const solar_os_terminal_t *terminal)
{
    return terminal != NULL && terminal->underline_enabled;
}

void solar_os_terminal_set_inverse(solar_os_terminal_t *terminal, bool enabled)
{
    if (terminal == NULL) {
        return;
    }

    terminal->inverse_enabled = enabled;
}

bool solar_os_terminal_inverse(const solar_os_terminal_t *terminal)
{
    return terminal != NULL && terminal->inverse_enabled;
}

void solar_os_terminal_write_bold(solar_os_terminal_t *terminal, const char *text)
{
    if (terminal == NULL) {
        return;
    }

    const bool previous = terminal->bold_enabled;
    terminal->bold_enabled = true;
    solar_os_terminal_write(terminal, text);
    terminal->bold_enabled = previous;
}

void solar_os_terminal_writeln_bold(solar_os_terminal_t *terminal, const char *text)
{
    solar_os_terminal_write_bold(terminal, text);
    solar_os_terminal_put_char(terminal, '\n');
}

void solar_os_terminal_printf_bold(solar_os_terminal_t *terminal, const char *fmt, ...)
{
    char buffer[160];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    solar_os_terminal_write_bold(terminal, buffer);
}

static bool terminal_status_bar_equal(const solar_os_status_bar_t *a,
                                      const solar_os_status_bar_t *b)
{
    return a->battery_valid == b->battery_valid &&
        a->battery_percent == b->battery_percent &&
        a->battery_external_power == b->battery_external_power &&
        a->ble_connected == b->ble_connected &&
        a->ble_scanning == b->ble_scanning &&
        a->wifi_started == b->wifi_started &&
        a->wifi_connected == b->wifi_connected &&
        a->wifi_has_ip == b->wifi_has_ip &&
        a->wifi_level == b->wifi_level &&
        a->audio_enabled == b->audio_enabled &&
        a->audio_volume == b->audio_volume &&
        a->time_valid == b->time_valid &&
        a->hour == b->hour &&
        a->minute == b->minute &&
        a->sd_mounted == b->sd_mounted;
}

void solar_os_terminal_set_status_bar(solar_os_terminal_t *terminal,
                                      const solar_os_status_bar_t *status)
{
    if (terminal == NULL || status == NULL ||
        terminal_status_bar_equal(&terminal->status_bar, status)) {
        return;
    }

    terminal->status_bar = *status;
    solar_os_terminal_mark_dirty(terminal);
}

void solar_os_terminal_get_status_bar(const solar_os_terminal_t *terminal,
                                      solar_os_status_bar_t *status)
{
    if (terminal == NULL || status == NULL) {
        return;
    }

    *status = terminal->status_bar;
}

size_t solar_os_terminal_cursor_col(const solar_os_terminal_t *terminal)
{
    if (terminal == NULL) {
        return 0;
    }

    return terminal->cursor_col;
}

size_t solar_os_terminal_cols(const solar_os_terminal_t *terminal)
{
    return terminal_cols(terminal);
}

size_t solar_os_terminal_cursor_row(const solar_os_terminal_t *terminal)
{
    if (terminal == NULL) {
        return 0;
    }

    return terminal->cursor_row;
}

size_t solar_os_terminal_rows(const solar_os_terminal_t *terminal)
{
    return terminal_rows(terminal);
}

void solar_os_terminal_set_cursor(solar_os_terminal_t *terminal, size_t row, size_t col)
{
    if (terminal == NULL) {
        return;
    }

    const size_t rows = terminal_rows(terminal);
    const size_t cols = terminal_cols(terminal);

    terminal_return_to_live(terminal);
    if (row >= rows) {
        row = rows - 1;
    }
    if (col > cols) {
        col = cols;
    }

    terminal->cursor_row = row;
    terminal->cursor_col = col;
    solar_os_terminal_mark_dirty(terminal);
}

void solar_os_terminal_set_cursor_visible(solar_os_terminal_t *terminal, bool visible)
{
    if (terminal == NULL || terminal->cursor_visible == visible) {
        return;
    }

    terminal->cursor_visible = visible;
    solar_os_terminal_mark_dirty(terminal);
}

bool solar_os_terminal_cursor_visible(const solar_os_terminal_t *terminal)
{
    return terminal != NULL && terminal->cursor_visible;
}

void solar_os_terminal_clear_primitives(solar_os_terminal_t *terminal)
{
    if (terminal == NULL || terminal->vrule_count == 0) {
        return;
    }

    terminal->vrule_count = 0;
    solar_os_terminal_mark_dirty(terminal);
}

esp_err_t solar_os_terminal_add_vrule(solar_os_terminal_t *terminal,
                                      size_t row,
                                      size_t col,
                                      size_t height,
                                      uint8_t width,
                                      bool inverse)
{
    if (terminal == NULL || height == 0 || row >= terminal_rows(terminal) ||
        col >= terminal_cols(terminal)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (terminal->vrule_count >= SOLAR_OS_TERMINAL_MAX_VRULES) {
        return ESP_ERR_NO_MEM;
    }

    solar_os_terminal_vrule_t *rule = &terminal->vrules[terminal->vrule_count++];
    rule->row = row;
    rule->col = col;
    rule->height = height;
    rule->width = width == 0 ? 1 : width;
    rule->inverse = inverse;
    solar_os_terminal_mark_dirty(terminal);
    return ESP_OK;
}

void solar_os_terminal_clear_line_from(solar_os_terminal_t *terminal, size_t row, size_t col)
{
    if (terminal == NULL || row >= terminal_rows(terminal) || col > terminal_cols(terminal)) {
        return;
    }

    terminal_return_to_live(terminal);
    if (terminal->bold_enabled ||
        terminal->italic_enabled ||
        terminal->underline_enabled ||
        terminal->inverse_enabled) {
        const size_t cols = terminal_cols(terminal);
        for (size_t i = col; i < cols; i++) {
            terminal->lines[row][i] = ' ';
            terminal_bold_set(terminal->bold[row], i, terminal->bold_enabled);
            terminal_italic_set(terminal->italic[row], i, terminal->italic_enabled);
            terminal_underline_set(terminal->underline[row], i, terminal->underline_enabled);
            terminal_inverse_set(terminal->inverse[row], i, terminal->inverse_enabled);
        }
        terminal->lines[row][cols] = 0;
    } else {
        memset(&terminal->lines[row][col],
               0,
               sizeof(terminal->lines[row][0]) * (SOLAR_OS_TERMINAL_MAX_COLS + 1 - col));
        terminal_bold_clear_from(terminal->bold[row], col);
        terminal_italic_clear_from(terminal->italic[row], col);
        terminal_underline_clear_from(terminal->underline[row], col);
        terminal_inverse_clear_from(terminal->inverse[row], col);
    }
    solar_os_terminal_mark_dirty(terminal);
}

void solar_os_terminal_page_up(solar_os_terminal_t *terminal)
{
    if (terminal == NULL || terminal->scrollback_count == 0) {
        return;
    }

    const size_t rows = terminal_rows(terminal);
    if (terminal->scrollback_count - terminal->scrollback_offset <= rows) {
        terminal->scrollback_offset = terminal->scrollback_count;
    } else {
        terminal->scrollback_offset += rows;
    }
    solar_os_terminal_mark_dirty(terminal);
}

void solar_os_terminal_page_down(solar_os_terminal_t *terminal)
{
    if (terminal == NULL || terminal->scrollback_offset == 0) {
        return;
    }

    const size_t rows = terminal_rows(terminal);
    if (terminal->scrollback_offset <= rows) {
        terminal->scrollback_offset = 0;
    } else {
        terminal->scrollback_offset -= rows;
    }
    solar_os_terminal_mark_dirty(terminal);
}

void solar_os_terminal_scroll_to_live(solar_os_terminal_t *terminal)
{
    terminal_return_to_live(terminal);
}

bool solar_os_terminal_is_scrolled_back(const solar_os_terminal_t *terminal)
{
    return terminal != NULL && terminal->scrollback_offset != 0;
}

uint16_t solar_os_terminal_orientation(const solar_os_terminal_t *terminal)
{
    return terminal != NULL ? terminal->orientation_degrees : 0;
}

esp_err_t solar_os_terminal_set_orientation(solar_os_terminal_t *terminal, uint16_t degrees)
{
    if (terminal == NULL || !terminal_orientation_is_valid(degrees)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (terminal->orientation_degrees == degrees) {
        return ESP_OK;
    }

    terminal->orientation_degrees = degrees;
    terminal_apply_settings(terminal, true);
    return terminal_save_u16(TERM_NVS_ORIENTATION_KEY, degrees);
}

solar_os_terminal_font_t solar_os_terminal_font(const solar_os_terminal_t *terminal)
{
    return terminal != NULL ? terminal->font : SOLAR_OS_TERMINAL_FONT_MONO;
}

esp_err_t solar_os_terminal_set_font(solar_os_terminal_t *terminal, solar_os_terminal_font_t font)
{
    if (terminal == NULL ||
        (size_t)font >= sizeof(terminal_font_families) / sizeof(terminal_font_families[0])) {
        return ESP_ERR_INVALID_ARG;
    }
    if (terminal->font == font) {
        return ESP_OK;
    }

    terminal->font = font;
    terminal_apply_settings(terminal, true);
    return terminal_save_u16(TERM_NVS_FONT_KEY, (uint16_t)font);
}

const char *solar_os_terminal_font_name(solar_os_terminal_font_t font)
{
    if ((size_t)font >= sizeof(terminal_font_families) / sizeof(terminal_font_families[0])) {
        return "unknown";
    }

    return terminal_font_families[font].name;
}

bool solar_os_terminal_parse_font(const char *name, solar_os_terminal_font_t *font)
{
    if (name == NULL || font == NULL) {
        return false;
    }

    for (size_t i = 0; i < sizeof(terminal_font_families) / sizeof(terminal_font_families[0]); i++) {
        if (strcmp(name, terminal_font_families[i].name) == 0) {
            *font = (solar_os_terminal_font_t)i;
            return true;
        }
    }

    return false;
}

solar_os_terminal_text_size_t solar_os_terminal_text_size(const solar_os_terminal_t *terminal)
{
    return terminal != NULL ? terminal->text_size : SOLAR_OS_TERMINAL_TEXT_SIZE_14;
}

esp_err_t solar_os_terminal_set_text_size(solar_os_terminal_t *terminal,
                                          solar_os_terminal_text_size_t text_size)
{
    if (terminal == NULL || !terminal_text_size_is_valid(text_size)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (terminal->text_size == text_size) {
        return ESP_OK;
    }

    terminal->text_size = text_size;
    terminal_apply_settings(terminal, true);
    return terminal_save_u16(TERM_NVS_TEXT_SIZE_KEY, (uint16_t)text_size);
}

esp_err_t solar_os_terminal_set_text_size_transient(solar_os_terminal_t *terminal,
                                                    solar_os_terminal_text_size_t text_size)
{
    if (terminal == NULL || !terminal_text_size_is_valid(text_size)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (terminal->text_size == text_size) {
        return ESP_OK;
    }

    terminal->text_size = text_size;
    terminal_apply_settings(terminal, true);
    return ESP_OK;
}

const char *solar_os_terminal_text_size_name(solar_os_terminal_text_size_t text_size)
{
    if (!terminal_text_size_is_valid(text_size)) {
        return "unknown";
    }

    return terminal_text_sizes[text_size].name;
}

bool solar_os_terminal_parse_text_size(const char *name, solar_os_terminal_text_size_t *text_size)
{
    if (name == NULL || text_size == NULL) {
        return false;
    }

    if (strcmp(name, "normal") == 0) {
        *text_size = SOLAR_OS_TERMINAL_TEXT_SIZE_14;
        return true;
    }
    if (strcmp(name, "large") == 0) {
        *text_size = SOLAR_OS_TERMINAL_TEXT_SIZE_20;
        return true;
    }

    for (size_t i = 0; i < sizeof(terminal_text_sizes) / sizeof(terminal_text_sizes[0]); i++) {
        if (terminal_text_sizes[i].name != NULL && strcmp(name, terminal_text_sizes[i].name) == 0) {
            *text_size = (solar_os_terminal_text_size_t)i;
            return true;
        }
    }

    return false;
}

bool solar_os_terminal_needs_draw(const solar_os_terminal_t *terminal)
{
    return terminal != NULL && terminal->dirty;
}

static bool terminal_box_segments(uint16_t cell, bool *left, bool *right, bool *up, bool *down)
{
    *left = false;
    *right = false;
    *up = false;
    *down = false;

    switch (cell) {
    case 0x2500:
    case 0x2501:
    case 0x2550:
        *left = true;
        *right = true;
        return true;
    case 0x2502:
    case 0x2503:
    case 0x2551:
        *up = true;
        *down = true;
        return true;
    case 0x250c:
    case 0x250f:
    case 0x2554:
        *right = true;
        *down = true;
        return true;
    case 0x2510:
    case 0x2513:
    case 0x2557:
        *left = true;
        *down = true;
        return true;
    case 0x2514:
    case 0x2517:
    case 0x255a:
        *right = true;
        *up = true;
        return true;
    case 0x2518:
    case 0x251b:
    case 0x255d:
        *left = true;
        *up = true;
        return true;
    case 0x251c:
    case 0x2523:
    case 0x2560:
        *up = true;
        *right = true;
        *down = true;
        return true;
    case 0x2524:
    case 0x252b:
    case 0x2563:
        *left = true;
        *up = true;
        *down = true;
        return true;
    case 0x252c:
    case 0x2533:
    case 0x2566:
        *left = true;
        *right = true;
        *down = true;
        return true;
    case 0x2534:
    case 0x253b:
    case 0x2569:
        *left = true;
        *right = true;
        *up = true;
        return true;
    case 0x253c:
    case 0x254b:
    case 0x256c:
        *left = true;
        *right = true;
        *up = true;
        *down = true;
        return true;
    default:
        return false;
    }
}

static bool terminal_draw_box_cell(u8g2_t *u8g2,
                                   int x,
                                   int y,
                                   int cell_width,
                                   int cell_height,
                                   uint16_t cell)
{
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
    if (!terminal_box_segments(cell, &left, &right, &up, &down)) {
        return false;
    }

    const int mid_x = x + (cell_width / 2);
    const int mid_y = y - (cell_height / 2);
    const int top_y = y - cell_height + 2;
    const int bottom_y = y + 1;
    if (left) {
        u8g2_DrawHLine(u8g2, (u8g2_uint_t)x, (u8g2_uint_t)mid_y, cell_width / 2 + 1);
    }
    if (right) {
        u8g2_DrawHLine(u8g2,
                       (u8g2_uint_t)mid_x,
                       (u8g2_uint_t)mid_y,
                       cell_width - (cell_width / 2));
    }
    if (up && mid_y > top_y) {
        u8g2_DrawVLine(u8g2, (u8g2_uint_t)mid_x, (u8g2_uint_t)top_y, mid_y - top_y + 1);
    }
    if (down && bottom_y > mid_y) {
        u8g2_DrawVLine(u8g2, (u8g2_uint_t)mid_x, (u8g2_uint_t)mid_y, bottom_y - mid_y + 1);
    }
    return true;
}

static bool terminal_draw_block_cell(u8g2_t *u8g2,
                                     int x,
                                     int y,
                                     int cell_width,
                                     int cell_height,
                                     uint16_t cell)
{
    const int top_y = y - cell_height + 2;
    const int half_height = cell_height / 2;

    if (cell >= 0x2581 && cell <= 0x2587) {
        const int fill_height = ((cell_height * (int)(cell - 0x2580)) + 7) / 8;
        u8g2_DrawBox(u8g2,
                     (u8g2_uint_t)x,
                     (u8g2_uint_t)(top_y + cell_height - fill_height),
                     cell_width,
                     fill_height);
        return true;
    }
    if (cell >= 0x2589 && cell <= 0x258f) {
        const int fill_width = ((cell_width * (int)(0x2590 - cell)) + 7) / 8;
        u8g2_DrawBox(u8g2, (u8g2_uint_t)x, (u8g2_uint_t)top_y, fill_width, cell_height);
        return true;
    }

    switch (cell) {
    case 0x2580:
        u8g2_DrawBox(u8g2, (u8g2_uint_t)x, (u8g2_uint_t)top_y, cell_width, half_height);
        return true;
    case 0x2584:
        u8g2_DrawBox(u8g2,
                     (u8g2_uint_t)x,
                     (u8g2_uint_t)(top_y + half_height),
                     cell_width,
                     cell_height - half_height);
        return true;
    case 0x2588:
        u8g2_DrawBox(u8g2, (u8g2_uint_t)x, (u8g2_uint_t)top_y, cell_width, cell_height);
        return true;
    case 0x2590:
        u8g2_DrawBox(u8g2,
                     (u8g2_uint_t)(x + (cell_width / 2)),
                     (u8g2_uint_t)top_y,
                     cell_width - (cell_width / 2),
                     cell_height);
        return true;
    case 0x2594:
        u8g2_DrawHLine(u8g2, (u8g2_uint_t)x, (u8g2_uint_t)top_y, cell_width);
        return true;
    case 0x2595:
        u8g2_DrawVLine(u8g2,
                       (u8g2_uint_t)(x + cell_width - 1),
                       (u8g2_uint_t)top_y,
                       cell_height);
        return true;
    case 0x2591:
    case 0x2592:
    case 0x2593:
        for (int yy = top_y; yy < top_y + cell_height; yy += 2) {
            for (int xx = x + ((yy + cell) & 1); xx < x + cell_width; xx += 2) {
                u8g2_DrawPixel(u8g2, (u8g2_uint_t)xx, (u8g2_uint_t)yy);
            }
        }
        return true;
    default:
        return false;
    }
}

static uint8_t terminal_apply_polarity(const solar_os_terminal_t *terminal, uint8_t white_bit)
{
    return terminal != NULL && terminal->black_is_one ? (uint8_t)!white_bit : white_bit;
}

static void terminal_set_draw_color(const solar_os_terminal_t *terminal,
                                    u8g2_t *u8g2,
                                    uint8_t white_bit)
{
    u8g2_SetDrawColor(u8g2, terminal_apply_polarity(terminal, white_bit));
}

static void terminal_draw_cell(solar_os_terminal_t *terminal,
                               u8g2_t *u8g2,
                               uint8_t text_scale,
                               int x,
                               int y,
                               uint16_t cell,
                               bool bold,
                               bool italic,
                               bool underline,
                               bool inverse)
{
    if (inverse) {
        int top_y = y - terminal->line_height + 2;
        int height = terminal->line_height;
        if (top_y < 0) {
            height += top_y;
            top_y = 0;
        }
        terminal_set_draw_color(terminal, u8g2, 0);
        if (height > 0) {
            u8g2_DrawBox(u8g2,
                         (u8g2_uint_t)x,
                         (u8g2_uint_t)top_y,
                         terminal->char_width,
                         height);
        }
        terminal_set_draw_color(terminal, u8g2, 1);
    } else {
        terminal_set_draw_color(terminal, u8g2, 0);
    }

    if (cell == 0) {
        terminal_set_draw_color(terminal, u8g2, 0);
        return;
    }
    if (terminal_draw_box_cell(u8g2, x, y, terminal->char_width, terminal->line_height, cell) ||
        terminal_draw_block_cell(u8g2, x, y, terminal->char_width, terminal->line_height, cell)) {
        terminal_set_draw_color(terminal, u8g2, 0);
        return;
    }

    u8g2_SetFont(u8g2, terminal_selected_font(terminal, bold, italic));
    if (text_scale == 2) {
        (void)u8g2_DrawGlyphX2(u8g2, (u8g2_uint_t)x, (u8g2_uint_t)y, cell);
    } else {
        (void)u8g2_DrawGlyph(u8g2, (u8g2_uint_t)x, (u8g2_uint_t)y, cell);
    }
    if (underline) {
        u8g2_DrawHLine(u8g2,
                       (u8g2_uint_t)x,
                       (u8g2_uint_t)(y + 1),
                       terminal->char_width);
    }
    terminal_set_draw_color(terminal, u8g2, 0);
}

static void terminal_draw_line(solar_os_terminal_t *terminal,
                               u8g2_t *u8g2,
                               uint8_t text_scale,
                               u8g2_uint_t y,
                               const solar_os_terminal_cell_t *line,
                               const uint32_t bold[SOLAR_OS_TERMINAL_ATTR_WORDS],
                               const uint32_t italic[SOLAR_OS_TERMINAL_ATTR_WORDS],
                               const uint32_t underline[SOLAR_OS_TERMINAL_ATTR_WORDS],
                               const uint32_t inverse[SOLAR_OS_TERMINAL_ATTR_WORDS])
{
    const size_t line_len = terminal_line_len(terminal, line);

    for (size_t pos = 0; pos < line_len; pos++) {
        const int x = TERM_MARGIN_X + (int)(pos * terminal->char_width);
        terminal_draw_cell(terminal,
                           u8g2,
                           text_scale,
                           x,
                           (int)y,
                           line[pos],
                           bold != NULL && terminal_bold_get(bold, pos),
                           italic != NULL && terminal_italic_get(italic, pos),
                           underline != NULL && terminal_underline_get(underline, pos),
                           inverse != NULL && terminal_inverse_get(inverse, pos));
    }
}

static int terminal_cell_top_y(const solar_os_terminal_t *terminal, size_t row)
{
    return (int)terminal->baseline_offset + (int)(row * terminal->line_height) -
        (int)terminal->line_height + 2;
}

static void terminal_draw_vrules(solar_os_terminal_t *terminal, u8g2_t *u8g2)
{
    const size_t rows = terminal_rows(terminal);
    const size_t cols = terminal_cols(terminal);
    const int display_width = u8g2_GetDisplayWidth(u8g2);
    const int display_height = u8g2_GetDisplayHeight(u8g2);

    for (size_t i = 0; i < terminal->vrule_count; i++) {
        const solar_os_terminal_vrule_t *rule = &terminal->vrules[i];
        if (rule->row >= rows || rule->col >= cols || rule->height == 0) {
            continue;
        }

        const size_t clipped_rows =
            rule->row + rule->height > rows ? rows - rule->row : rule->height;
        if (clipped_rows == 0) {
            continue;
        }

        const int width = rule->width == 0 ? 1 : rule->width;
        int x = TERM_MARGIN_X + (int)(rule->col * terminal->char_width) +
            (terminal->char_width / 2) - (width / 2);
        int y = terminal_cell_top_y(terminal, rule->row);
        int height = (int)(clipped_rows * terminal->line_height);
        if (y < 0) {
            height += y;
            y = 0;
        }
        if (y + height > display_height) {
            height = display_height - y;
        }
        if (height <= 0) {
            continue;
        }
        if (x < 0) {
            x = 0;
        }
        if (x >= display_width) {
            continue;
        }

        int draw_width = width;
        if (x + draw_width > display_width) {
            draw_width = display_width - x;
        }
        if (draw_width <= 0) {
            continue;
        }

        terminal_set_draw_color(terminal, u8g2, rule->inverse ? 1 : 0);
        u8g2_DrawBox(u8g2,
                     (u8g2_uint_t)x,
                     (u8g2_uint_t)y,
                     (u8g2_uint_t)draw_width,
                     (u8g2_uint_t)height);
    }
    terminal_set_draw_color(terminal, u8g2, 0);
}

static int cursor_x_position(solar_os_terminal_t *terminal)
{
    if (terminal == NULL || terminal->cursor_row >= terminal_rows(terminal)) {
        return TERM_MARGIN_X;
    }

    size_t cursor_col = terminal->cursor_col;
    if (cursor_col > terminal_cols(terminal)) {
        cursor_col = terminal_cols(terminal);
    }

    return TERM_MARGIN_X + (int)(cursor_col * terminal->char_width);
}

static const solar_os_terminal_cell_t *terminal_display_line(const solar_os_terminal_t *terminal,
                                                             size_t row,
                                                             const uint32_t **bold,
                                                             const uint32_t **italic,
                                                             const uint32_t **underline,
                                                             const uint32_t **inverse)
{
    static const uint32_t no_attrs[SOLAR_OS_TERMINAL_ATTR_WORDS];
    static const solar_os_terminal_cell_t empty_line[SOLAR_OS_TERMINAL_MAX_COLS + 1];

    if (bold != NULL) {
        *bold = no_attrs;
    }
    if (italic != NULL) {
        *italic = no_attrs;
    }
    if (underline != NULL) {
        *underline = no_attrs;
    }
    if (inverse != NULL) {
        *inverse = no_attrs;
    }
    if (terminal == NULL) {
        return empty_line;
    }

    const size_t history_count = terminal->scrollback_count;
    const size_t offset = terminal->scrollback_offset <= history_count ?
        terminal->scrollback_offset :
        history_count;
    const size_t start = history_count - offset;
    const size_t virtual_index = start + row;

    if (virtual_index < history_count) {
        if (bold != NULL) {
            *bold = scrollback_bold_line(terminal, virtual_index);
        }
        if (italic != NULL) {
            *italic = scrollback_italic_line(terminal, virtual_index);
        }
        if (underline != NULL) {
            *underline = scrollback_underline_line(terminal, virtual_index);
        }
        if (inverse != NULL) {
            *inverse = scrollback_inverse_line(terminal, virtual_index);
        }
        const solar_os_terminal_cell_t *line = scrollback_line(terminal, virtual_index);
        return line != NULL ? line : empty_line;
    }

    const size_t live_row = virtual_index - history_count;
    if (live_row < terminal_rows(terminal)) {
        if (bold != NULL) {
            *bold = terminal->bold[live_row];
        }
        if (italic != NULL) {
            *italic = terminal->italic[live_row];
        }
        if (underline != NULL) {
            *underline = terminal->underline[live_row];
        }
        if (inverse != NULL) {
            *inverse = terminal->inverse[live_row];
        }
        return terminal->lines[live_row];
    }

    return empty_line;
}

static void terminal_draw_status_slash(u8g2_t *u8g2, int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }

    const int denom = width > 1 ? width - 1 : 1;
    for (int px = 0; px < width; px++) {
        const int py = y + ((px * (height - 1)) / denom);
        u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + px), (u8g2_uint_t)py);
        if (py + 1 < y + height) {
            u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + px), (u8g2_uint_t)(py + 1));
        }
    }
}

static void terminal_draw_diag_down(u8g2_t *u8g2, int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }

    const int denom = width > 1 ? width - 1 : 1;
    for (int px = 0; px < width; px++) {
        const int py = y + ((px * (height - 1)) / denom);
        u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + px), (u8g2_uint_t)py);
    }
}

static void terminal_draw_diag_up(u8g2_t *u8g2, int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }

    const int denom = width > 1 ? width - 1 : 1;
    for (int px = 0; px < width; px++) {
        const int py = y + height - 1 - ((px * (height - 1)) / denom);
        u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + px), (u8g2_uint_t)py);
    }
}

static void terminal_draw_battery_icon(u8g2_t *u8g2,
                                       int x,
                                       int y,
                                       bool valid,
                                       uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    u8g2_DrawFrame(u8g2, (u8g2_uint_t)x, (u8g2_uint_t)y, 18, 10);
    u8g2_DrawBox(u8g2, (u8g2_uint_t)(x + 18), (u8g2_uint_t)(y + 3), 2, 4);

    if (!valid) {
        terminal_draw_status_slash(u8g2, x + 2, y + 1, 15, 8);
        return;
    }

    int fill_width = ((int)percent * 14 + 99) / 100;
    if (percent == 0) {
        fill_width = 0;
    }
    if (fill_width > 0) {
        u8g2_DrawBox(u8g2,
                     (u8g2_uint_t)(x + 2),
                     (u8g2_uint_t)(y + 2),
                     (u8g2_uint_t)fill_width,
                     6);
    }
}

static void terminal_draw_plug_icon(u8g2_t *u8g2, int x, int y)
{
    u8g2_DrawVLine(u8g2, (u8g2_uint_t)(x + 5), (u8g2_uint_t)y, 4);
    u8g2_DrawVLine(u8g2, (u8g2_uint_t)(x + 10), (u8g2_uint_t)y, 4);
    u8g2_DrawHLine(u8g2, (u8g2_uint_t)(x + 4), (u8g2_uint_t)(y + 4), 8);
    u8g2_DrawFrame(u8g2, (u8g2_uint_t)(x + 3), (u8g2_uint_t)(y + 4), 10, 7);
    u8g2_DrawBox(u8g2, (u8g2_uint_t)(x + 5), (u8g2_uint_t)(y + 6), 6, 4);
    u8g2_DrawHLine(u8g2, (u8g2_uint_t)(x + 13), (u8g2_uint_t)(y + 7), 4);
    u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + 17), (u8g2_uint_t)(y + 8));
    u8g2_DrawVLine(u8g2, (u8g2_uint_t)(x + 18), (u8g2_uint_t)(y + 8), 3);
    u8g2_DrawHLine(u8g2, (u8g2_uint_t)(x + 15), (u8g2_uint_t)(y + 11), 4);
}

static void terminal_draw_keyboard_icon(u8g2_t *u8g2,
                                        int x,
                                        int y,
                                        bool connected,
                                        bool scanning)
{
    u8g2_DrawFrame(u8g2, (u8g2_uint_t)x, (u8g2_uint_t)(y + 1), 18, 10);
    u8g2_DrawHLine(u8g2, (u8g2_uint_t)(x + 2), (u8g2_uint_t)(y + 4), 14);
    u8g2_DrawHLine(u8g2, (u8g2_uint_t)(x + 2), (u8g2_uint_t)(y + 7), 14);
    for (int key = 0; key < 4; key++) {
        u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + 3 + key * 4), (u8g2_uint_t)(y + 3));
        u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + 4 + key * 3), (u8g2_uint_t)(y + 6));
    }
    u8g2_DrawBox(u8g2, (u8g2_uint_t)(x + 5), (u8g2_uint_t)(y + 9), 8, 1);

    if (scanning) {
        u8g2_DrawFrame(u8g2, (u8g2_uint_t)(x + 15), (u8g2_uint_t)y, 4, 4);
        terminal_draw_diag_down(u8g2, x + 18, y + 3, 4, 4);
    } else if (!connected) {
        terminal_draw_status_slash(u8g2, x + 1, y + 2, 16, 8);
    }
}

static void terminal_draw_wifi_icon(u8g2_t *u8g2,
                                    int x,
                                    int y,
                                    bool started,
                                    bool connected,
                                    bool has_ip,
                                    uint8_t level)
{
    if (level > 3) {
        level = 3;
    }

    const int base_y = y + 11;
    for (int bar = 0; bar < 3; bar++) {
        const int height = 3 + (bar * 3);
        const int bar_x = x + (bar * 5);
        const int bar_y = base_y - height + 1;
        if (connected && has_ip && level > (uint8_t)bar) {
            u8g2_DrawBox(u8g2,
                         (u8g2_uint_t)bar_x,
                         (u8g2_uint_t)bar_y,
                         3,
                         (u8g2_uint_t)height);
        } else {
            u8g2_DrawFrame(u8g2,
                           (u8g2_uint_t)bar_x,
                           (u8g2_uint_t)bar_y,
                           3,
                           (u8g2_uint_t)height);
        }
    }

    if (!started) {
        terminal_draw_status_slash(u8g2, x, y + 2, 13, 9);
    }
}

static void terminal_draw_sd_icon(u8g2_t *u8g2, int x, int y, bool mounted)
{
    u8g2_DrawHLine(u8g2, (u8g2_uint_t)x, (u8g2_uint_t)y, 8);
    terminal_draw_diag_down(u8g2, x + 8, y, 5, 5);
    u8g2_DrawVLine(u8g2, (u8g2_uint_t)(x + 12), (u8g2_uint_t)(y + 4), 7);
    u8g2_DrawHLine(u8g2, (u8g2_uint_t)x, (u8g2_uint_t)(y + 10), 13);
    u8g2_DrawVLine(u8g2, (u8g2_uint_t)x, (u8g2_uint_t)y, 11);

    if (mounted) {
        u8g2_DrawBox(u8g2, (u8g2_uint_t)(x + 3), (u8g2_uint_t)(y + 3), 5, 2);
        u8g2_DrawHLine(u8g2, (u8g2_uint_t)(x + 3), (u8g2_uint_t)(y + 7), 7);
    } else {
        terminal_draw_status_slash(u8g2, x + 1, y + 2, 11, 8);
    }
}

static void terminal_draw_speaker_icon(u8g2_t *u8g2, int x, int y, bool enabled, uint8_t volume)
{
    const bool audible = enabled && volume > 0;
    const uint8_t bars = audible ? (volume > 80 ? 2 : 1) : 0;

    u8g2_DrawBox(u8g2, (u8g2_uint_t)x, (u8g2_uint_t)(y + 5), 3, 4);
    terminal_draw_diag_up(u8g2, x + 3, y + 2, 6, 4);
    terminal_draw_diag_down(u8g2, x + 3, y + 8, 6, 4);
    u8g2_DrawVLine(u8g2, (u8g2_uint_t)(x + 8), (u8g2_uint_t)(y + 2), 10);

    if (bars >= 1) {
        u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + 11), (u8g2_uint_t)(y + 4));
        u8g2_DrawVLine(u8g2, (u8g2_uint_t)(x + 12), (u8g2_uint_t)(y + 5), 4);
        u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + 11), (u8g2_uint_t)(y + 9));
    }

    if (bars >= 2) {
        u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + 15), (u8g2_uint_t)(y + 2));
        u8g2_DrawVLine(u8g2, (u8g2_uint_t)(x + 16), (u8g2_uint_t)(y + 3), 8);
        u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + 15), (u8g2_uint_t)(y + 11));
    }

    if (!audible) {
        terminal_draw_status_slash(u8g2, x, y + 2, 17, 9);
    }
}

static void terminal_draw_clock_icon(u8g2_t *u8g2, int x, int y, bool valid)
{
    static const int8_t face[][2] = {
        {0, -4},
        {1, -4},
        {2, -3},
        {3, -2},
        {4, -1},
        {4, 0},
        {4, 1},
        {3, 2},
        {2, 3},
        {1, 4},
        {0, 4},
        {-1, 4},
        {-2, 3},
        {-3, 2},
        {-4, 1},
        {-4, 0},
        {-4, -1},
        {-3, -2},
        {-2, -3},
        {-1, -4},
    };
    const int cx = x + 4;
    const int cy = y + 5;
    for (size_t i = 0; i < sizeof(face) / sizeof(face[0]); i++) {
        u8g2_DrawPixel(u8g2,
                       (u8g2_uint_t)(cx + face[i][0]),
                       (u8g2_uint_t)(cy + face[i][1]));
    }
    u8g2_DrawVLine(u8g2, (u8g2_uint_t)cx, (u8g2_uint_t)(cy - 3), 4);
    u8g2_DrawHLine(u8g2, (u8g2_uint_t)cx, (u8g2_uint_t)cy, 3);
    if (!valid) {
        terminal_draw_status_slash(u8g2, x, y + 1, 9, 8);
    }
}

static void terminal_draw_status_bar(solar_os_terminal_t *terminal, u8g2_t *u8g2)
{
    const solar_os_status_bar_t *status = &terminal->status_bar;
    const int width = u8g2_GetDisplayWidth(u8g2);
    const int height =
        terminal->status_bar_height > 0 ? terminal->status_bar_height : TERM_STATUS_BAR_HEIGHT;

    terminal_set_draw_color(terminal, u8g2, 0);
    u8g2_DrawBox(u8g2, 0, 0, (u8g2_uint_t)width, (u8g2_uint_t)height);

    terminal_set_draw_color(terminal, u8g2, 1);
    const int icon_y = height >= 16 ? 3 : 1;
    int x = 4;

    if (status->battery_external_power) {
        terminal_draw_plug_icon(u8g2, x, icon_y);
    } else {
        terminal_draw_battery_icon(u8g2, x, icon_y, status->battery_valid, status->battery_percent);
    }
    x += 28;
    terminal_draw_keyboard_icon(u8g2, x, icon_y, status->ble_connected, status->ble_scanning);
    x += 26;
    terminal_draw_wifi_icon(u8g2,
                            x,
                            icon_y,
                            status->wifi_started,
                            status->wifi_connected,
                            status->wifi_has_ip,
                            status->wifi_level);
    x += 22;
    terminal_draw_speaker_icon(u8g2, x, icon_y, status->audio_enabled, status->audio_volume);
    x += 24;

    u8g2_SetFont(u8g2, u8g2_font_solar_os_default_r_12_tf);
    u8g2_SetFontMode(u8g2, 1);
    u8g2_SetFontPosBaseline(u8g2);

    char time_text[6];
    if (status->time_valid) {
        snprintf(time_text,
                 sizeof(time_text),
                 "%02u:%02u",
                 (unsigned)status->hour,
                 (unsigned)status->minute);
    } else {
        strlcpy(time_text, "--:--", sizeof(time_text));
    }

    const int time_width = (int)u8g2_GetStrWidth(u8g2, time_text);
    const int time_x = width - time_width - 4;
    const int clock_x = time_x - 12;

    if (x + 15 < clock_x) {
        terminal_draw_sd_icon(u8g2, x, icon_y, status->sd_mounted);
        x += 20;
    }
    if (x + 10 < clock_x) {
        terminal_draw_clock_icon(u8g2, clock_x, icon_y, status->time_valid);
    }

    u8g2_DrawStr(u8g2,
                 (u8g2_uint_t)time_x,
                 (u8g2_uint_t)(height - 3),
                 time_text);
    terminal_set_draw_color(terminal, u8g2, 0);
}

void solar_os_terminal_draw(solar_os_terminal_t *terminal)
{
    if (terminal == NULL || terminal->u8g2 == NULL) {
        return;
    }

    u8g2_t *u8g2 = terminal->u8g2;
    (void)solar_os_display_request_present_mode(u8g2, SOLAR_OS_DISPLAY_PRESENT_TEXT);
    terminal_apply_settings(terminal, false);

    u8g2_ClearBuffer(u8g2);
    terminal_set_draw_color(terminal, u8g2, 1);
    u8g2_DrawBox(u8g2, 0, 0, u8g2_GetDisplayWidth(u8g2), u8g2_GetDisplayHeight(u8g2));
    terminal_draw_status_bar(terminal, u8g2);
    terminal_set_draw_color(terminal, u8g2, 0);
    u8g2_SetFontMode(u8g2, 1);
    u8g2_SetFontPosBaseline(u8g2);
    const uint8_t text_scale = terminal_text_scale(terminal);

    for (size_t row = 0; row < terminal_rows(terminal); row++) {
        const int y = terminal->baseline_offset + (int)(row * terminal->line_height);
        const uint32_t *bold = NULL;
        const uint32_t *italic = NULL;
        const uint32_t *underline = NULL;
        const uint32_t *inverse = NULL;
        const solar_os_terminal_cell_t *line =
            terminal_display_line(terminal, row, &bold, &italic, &underline, &inverse);
        terminal_draw_line(terminal,
                           u8g2,
                           text_scale,
                           (u8g2_uint_t)y,
                           line,
                           bold,
                           italic,
                           underline,
                           inverse);
    }

    terminal_draw_vrules(terminal, u8g2);

    if (terminal->cursor_visible && !solar_os_terminal_is_scrolled_back(terminal)) {
        const int cursor_x = cursor_x_position(terminal);
        const int cursor_y = terminal_cell_top_y(terminal, terminal->cursor_row) +
            (int)terminal->line_height - 2;
        const bool inverse_cursor =
            terminal->cursor_row < terminal_rows(terminal) &&
            terminal->cursor_col < terminal_cols(terminal) &&
            terminal_inverse_get(terminal->inverse[terminal->cursor_row], terminal->cursor_col);
        terminal_set_draw_color(terminal, u8g2, inverse_cursor ? 1 : 0);
        u8g2_DrawHLine(u8g2, (u8g2_uint_t)cursor_x, (u8g2_uint_t)cursor_y, terminal->char_width);
        terminal_set_draw_color(terminal, u8g2, 0);
    }

    u8g2_SendBuffer(u8g2);
    terminal->dirty = false;
}
