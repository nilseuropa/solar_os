#include "solar_os_edit.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_board_caps.h"
#include "solar_os_clipboard.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
#include "solar_os_syntax.h"
#include "solar_os_terminal.h"
#include "solar_os_text_search.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define EDITOR_PSRAM_BUFFER_CAPACITY (256 * 1024)
#define EDITOR_INTERNAL_BUFFER_CAPACITY (32 * 1024)
#define EDITOR_TAB_WIDTH 4

typedef enum {
    EDITOR_MODE_TEXT = 0,
    EDITOR_MODE_HEX,
} editor_mode_t;

typedef enum {
    EDITOR_HEX_PANE_HEX = 0,
    EDITOR_HEX_PANE_ASCII,
} editor_hex_pane_t;

typedef struct {
    solar_os_tui_t tui;
    char *buffer;
    size_t len;
    size_t capacity;
    size_t cursor;
    size_t preferred_col;
    size_t top_line;
    size_t left_col;
    size_t selection_anchor;
    bool dirty;
    bool selection_active;
    bool saved_text_size_valid;
    editor_mode_t mode;
    editor_hex_pane_t hex_pane;
    uint8_t hex_nibble;
    solar_os_terminal_text_size_t saved_text_size;
    solar_os_syntax_language_t syntax;
    solar_os_text_search_state_t search;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char display_name[SOLAR_OS_STORAGE_PATH_MAX];
    char message[72];
} editor_state_t;

static void *editor_state;
#define editor (*(editor_state_t *)editor_state)

static const solar_os_terminal_text_size_t editor_text_sizes[] = {
    SOLAR_OS_TERMINAL_TEXT_SIZE_10,
    SOLAR_OS_TERMINAL_TEXT_SIZE_12,
    SOLAR_OS_TERMINAL_TEXT_SIZE_14,
    SOLAR_OS_TERMINAL_TEXT_SIZE_16,
    SOLAR_OS_TERMINAL_TEXT_SIZE_18,
    SOLAR_OS_TERMINAL_TEXT_SIZE_20,
};

static bool editor_is_printable(char ch)
{
    const unsigned char value = (unsigned char)ch;

    return isprint(value) || value >= 0xa0;
}

static const char *editor_app_name(void)
{
    return editor.mode == EDITOR_MODE_HEX ? "hexedit" : "edit";
}

static size_t editor_line_start_for(size_t index)
{
    if (index > editor.len) {
        index = editor.len;
    }

    while (index > 0 && editor.buffer[index - 1] != '\n') {
        index--;
    }
    return index;
}

static size_t editor_line_end_for(size_t start)
{
    size_t end = start;

    while (end < editor.len && editor.buffer[end] != '\n') {
        end++;
    }
    return end;
}

static size_t editor_line_for_index(size_t index)
{
    size_t line = 0;

    if (index > editor.len) {
        index = editor.len;
    }

    for (size_t i = 0; i < index; i++) {
        if (editor.buffer[i] == '\n') {
            line++;
        }
    }
    return line;
}

static size_t editor_index_for_line(size_t line)
{
    if (line == 0) {
        return 0;
    }

    size_t current_line = 0;
    for (size_t i = 0; i < editor.len; i++) {
        if (editor.buffer[i] != '\n') {
            continue;
        }
        current_line++;
        if (current_line == line) {
            return i + 1;
        }
    }

    return editor.len;
}

static size_t editor_cursor_col(void)
{
    return editor.cursor - editor_line_start_for(editor.cursor);
}

static void editor_update_preferred_col(void)
{
    editor.preferred_col = editor_cursor_col();
}

static void editor_update_cursor_column(void)
{
    if (editor.mode == EDITOR_MODE_TEXT) {
        editor_update_preferred_col();
    } else {
        editor.hex_nibble = 0;
    }
}

static void editor_set_message(const char *message)
{
    strlcpy(editor.message, message != NULL ? message : "", sizeof(editor.message));
}

static void editor_set_capacity_message(const char *message)
{
    snprintf(editor.message,
             sizeof(editor.message),
             "%s (%u KiB)",
             message,
             (unsigned)(editor.capacity / 1024U));
}

static void editor_capture_text_size(void)
{
    solar_os_terminal_t *terminal = editor.tui.terminal;
    if (terminal == NULL) {
        return;
    }

    editor.saved_text_size = solar_os_terminal_text_size(terminal);
    editor.saved_text_size_valid = true;
}

static void editor_restore_text_size(void)
{
    if (!editor.saved_text_size_valid) {
        return;
    }

    solar_os_terminal_t *terminal = editor.tui.terminal;
    if (terminal != NULL) {
        (void)solar_os_terminal_set_text_size_transient(terminal, editor.saved_text_size);
    }
}

static int editor_text_size_index(solar_os_terminal_text_size_t text_size)
{
    for (size_t i = 0; i < sizeof(editor_text_sizes) / sizeof(editor_text_sizes[0]); i++) {
        if (editor_text_sizes[i] == text_size) {
            return (int)i;
        }
    }
    return 1;
}

static void editor_adjust_text_size(int delta)
{
    solar_os_terminal_t *terminal = editor.tui.terminal;
    if (terminal == NULL) {
        editor_set_message("text size display only");
        return;
    }

    int index = editor_text_size_index(solar_os_terminal_text_size(terminal));
    index += delta;
    if (index < 0) {
        index = 0;
    } else if (index >= (int)(sizeof(editor_text_sizes) / sizeof(editor_text_sizes[0]))) {
        index = (int)(sizeof(editor_text_sizes) / sizeof(editor_text_sizes[0])) - 1;
    }

    const solar_os_terminal_text_size_t text_size = editor_text_sizes[index];
    const esp_err_t err = solar_os_terminal_set_text_size_transient(terminal, text_size);
    if (err != ESP_OK) {
        editor_set_message("text size failed");
        return;
    }

    char message[sizeof(editor.message)];
    snprintf(message,
             sizeof(message),
             "text size %s",
             solar_os_terminal_text_size_name(text_size));
    editor_set_message(message);
}

static bool editor_has_selection(void)
{
    return editor.selection_active && editor.selection_anchor != editor.cursor;
}

static void editor_selection_bounds(size_t *start, size_t *end)
{
    size_t first = editor.selection_anchor;
    size_t last = editor.cursor;

    if (first > last) {
        const size_t temp = first;
        first = last;
        last = temp;
    }
    if (first > editor.len) {
        first = editor.len;
    }
    if (last > editor.len) {
        last = editor.len;
    }

    if (start != NULL) {
        *start = first;
    }
    if (end != NULL) {
        *end = last;
    }
}

static void editor_clear_selection(void)
{
    editor.selection_active = false;
    editor.selection_anchor = editor.cursor;
}

static void editor_begin_selection(bool selecting)
{
    if (selecting && !editor.selection_active) {
        editor.selection_anchor = editor.cursor;
    }
}

static void editor_finish_selection(bool selecting)
{
    if (selecting) {
        editor.selection_active = editor.selection_anchor != editor.cursor;
        return;
    }

    editor_clear_selection();
}

static uint8_t editor_tui_attr(solar_os_syntax_style_t style, bool inverse)
{
    uint8_t attr = SOLAR_OS_TUI_ATTR_NORMAL;
    if (style == SOLAR_OS_SYNTAX_STYLE_KEYWORD) {
        attr |= SOLAR_OS_TUI_ATTR_BOLD;
    }
    if (style == SOLAR_OS_SYNTAX_STYLE_COMMENT) {
        attr |= SOLAR_OS_TUI_ATTR_UNDERLINE;
    }
    if (style == SOLAR_OS_SYNTAX_STYLE_STRING ||
        style == SOLAR_OS_SYNTAX_STYLE_NUMBER) {
        attr |= SOLAR_OS_TUI_ATTR_ITALIC;
    }
    if (inverse) {
        attr |= SOLAR_OS_TUI_ATTR_INVERSE;
    }
    return attr;
}

static void editor_prepare_syntax_state(solar_os_syntax_state_t *state, size_t first_line)
{
    solar_os_syntax_state_init(state);
    if (state == NULL || editor.syntax == SOLAR_OS_SYNTAX_NONE || first_line == 0) {
        return;
    }

    size_t start = 0;
    for (size_t line = 0; line < first_line && start < editor.len; line++) {
        const size_t end = editor_line_end_for(start);
        solar_os_syntax_highlight_line(editor.syntax,
                                       state,
                                       &editor.buffer[start],
                                       end - start,
                                       0,
                                       NULL,
                                       0);
        start = end < editor.len ? end + 1 : editor.len;
    }
}

static void editor_ensure_cursor_visible(size_t text_rows, size_t cols)
{
    const size_t visible_rows = text_rows > 0 ? text_rows : 1;
    const size_t cursor_line = editor_line_for_index(editor.cursor);
    const size_t cursor_col = editor_cursor_col();

    if (cursor_line < editor.top_line) {
        editor.top_line = cursor_line;
    } else if (cursor_line >= editor.top_line + visible_rows) {
        editor.top_line = cursor_line - visible_rows + 1;
    }

    if (cursor_col < editor.left_col) {
        editor.left_col = cursor_col;
    } else if (cursor_col >= editor.left_col + cols) {
        editor.left_col = cursor_col - cols + 1;
    }
}

static size_t editor_hex_bytes_per_row(size_t cols)
{
    if (cols >= 75U) {
        return 16U;
    }
    if (cols >= 43U) {
        return 8U;
    }
    if (cols >= 27U) {
        return 4U;
    }
    return 1U;
}

static void editor_hex_ensure_cursor_visible(size_t text_rows, size_t bytes_per_row)
{
    const size_t visible_rows = text_rows > 0 ? text_rows : 1U;
    const size_t cursor_row = editor.cursor / bytes_per_row;

    if (cursor_row < editor.top_line) {
        editor.top_line = cursor_row;
    } else if (cursor_row >= editor.top_line + visible_rows) {
        editor.top_line = cursor_row - visible_rows + 1U;
    }
}

static uint8_t editor_hex_cell_attr(size_t index,
                                    editor_hex_pane_t pane,
                                    bool has_selection,
                                    size_t selection_start,
                                    size_t selection_end)
{
    if (has_selection && index >= selection_start && index < selection_end) {
        return SOLAR_OS_TUI_ATTR_INVERSE;
    }
    if (index != editor.cursor) {
        return SOLAR_OS_TUI_ATTR_NORMAL;
    }
    if (pane == editor.hex_pane) {
        return SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD;
    }
    return SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_UNDERLINE;
}

static void editor_render_hex(void)
{
    const size_t rows = solar_os_tui_rows(&editor.tui);
    const size_t cols = solar_os_tui_cols(&editor.tui);
    const size_t text_rows = rows > 2U ? rows - 2U : 0U;
    const size_t bytes_per_row = editor_hex_bytes_per_row(cols);
    const size_t ascii_col = 9U + bytes_per_row * 3U + 2U;
    size_t selection_start = 0;
    size_t selection_end = 0;
    const bool has_selection = editor_has_selection();
    char header[192];

    if (rows == 0 || cols == 0) {
        return;
    }
    if (has_selection) {
        editor_selection_bounds(&selection_start, &selection_end);
    }

    editor_hex_ensure_cursor_visible(text_rows, bytes_per_row);
    solar_os_tui_clear(&editor.tui);
    (void)solar_os_tui_set_cursor_visible(&editor.tui, false);

    snprintf(header,
             sizeof(header),
             "hexedit %s%s  %s",
             editor.display_name,
             editor.dirty ? " *" : "",
             editor.hex_pane == EDITOR_HEX_PANE_HEX ? "[HEX] | ASCII" : "HEX | [ASCII]");
    solar_os_tui_write_cell(&editor.tui, 0, 0, cols, header,
                            SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);

    for (size_t row = 0; row < text_rows; row++) {
        const size_t data_row = editor.top_line + row;
        const size_t offset = data_row * bytes_per_row;
        if (offset > editor.len) {
            continue;
        }

        char address[12];
        snprintf(address, sizeof(address), "%08X", (unsigned)offset);
        solar_os_tui_write_cell(&editor.tui, row + 1U, 0, 8U, address,
                                SOLAR_OS_TUI_ATTR_BOLD);
        if (ascii_col - 2U < cols) {
            (void)solar_os_tui_putch(&editor.tui,
                                     row + 1U,
                                     ascii_col - 2U,
                                     '|',
                                     SOLAR_OS_TUI_ATTR_NORMAL);
        }

        for (size_t byte_col = 0; byte_col < bytes_per_row; byte_col++) {
            const size_t index = offset + byte_col;
            if (index > editor.len) {
                break;
            }
            const uint8_t hex_attr = editor_hex_cell_attr(index,
                                                           EDITOR_HEX_PANE_HEX,
                                                           has_selection,
                                                           selection_start,
                                                           selection_end);
            const uint8_t ascii_attr = editor_hex_cell_attr(index,
                                                             EDITOR_HEX_PANE_ASCII,
                                                             has_selection,
                                                             selection_start,
                                                             selection_end);
            const size_t hex_col = 9U + byte_col * 3U;
            if (index < editor.len) {
                char hex[3];
                const uint8_t value = (uint8_t)editor.buffer[index];
                snprintf(hex, sizeof(hex), "%02X", value);
                solar_os_tui_write_cell(&editor.tui, row + 1U, hex_col, 2U,
                                        hex, hex_attr);
                if (ascii_col + byte_col < cols) {
                    const uint32_t codepoint = editor_is_printable((char)value) ? value : '.';
                    (void)solar_os_tui_putch(&editor.tui,
                                             row + 1U,
                                             ascii_col + byte_col,
                                             codepoint,
                                             ascii_attr);
                }
            } else {
                solar_os_tui_write_cell(&editor.tui, row + 1U, hex_col, 2U,
                                        "  ", hex_attr);
                if (ascii_col + byte_col < cols) {
                    (void)solar_os_tui_putch(&editor.tui,
                                             row + 1U,
                                             ascii_col + byte_col,
                                             ' ',
                                             ascii_attr);
                }
            }
        }
    }

    if (rows > 1U) {
        char footer[192];
        char value[4] = "--";
        if (editor.cursor < editor.len) {
            snprintf(value, sizeof(value), "%02X", (uint8_t)editor.buffer[editor.cursor]);
        }
        if (editor.message[0] != '\0') {
            snprintf(footer,
                     sizeof(footer),
                     "Off %08X  %s  %s",
                     (unsigned)editor.cursor,
                     value,
                     editor.message);
        } else {
            snprintf(footer,
                     sizeof(footer),
                     "Off %08X  %s  %u/%u bytes  Tab pane  ^S save  ^Q/ESC quit",
                     (unsigned)editor.cursor,
                     value,
                     (unsigned)editor.len,
                     (unsigned)(editor.capacity - 1U));
        }
        solar_os_tui_draw_help(&editor.tui, footer);
    }

    if (text_rows > 0U) {
        const size_t cursor_row = editor.cursor / bytes_per_row;
        if (cursor_row >= editor.top_line &&
            cursor_row < editor.top_line + text_rows) {
            const size_t byte_col = editor.cursor % bytes_per_row;
            const size_t cursor_col = editor.hex_pane == EDITOR_HEX_PANE_HEX ?
                9U + byte_col * 3U + editor.hex_nibble :
                ascii_col + byte_col;
            if (cursor_col < cols) {
                (void)solar_os_tui_move(&editor.tui,
                                        cursor_row - editor.top_line + 1U,
                                        cursor_col);
                (void)solar_os_tui_set_cursor_visible(&editor.tui, true);
            }
        }
    }
    solar_os_tui_refresh(&editor.tui);
}

static void editor_render(solar_os_context_t *ctx)
{
    (void)ctx;

    const size_t rows = solar_os_tui_rows(&editor.tui);
    const size_t cols = solar_os_tui_cols(&editor.tui);
    const size_t text_rows = rows > 2 ? rows - 2 : 0;
    size_t cursor_line;
    size_t cursor_col;
    solar_os_syntax_state_t syntax_state;
    size_t selection_start = 0;
    size_t selection_end = 0;
    const bool has_selection = editor_has_selection();
    char header[192];

    if (rows == 0 || cols == 0) {
        return;
    }
    if (editor.mode == EDITOR_MODE_HEX) {
        editor_render_hex();
        return;
    }

    cursor_line = editor_line_for_index(editor.cursor);
    cursor_col = editor_cursor_col();

    if (has_selection) {
        editor_selection_bounds(&selection_start, &selection_end);
    }

    editor_ensure_cursor_visible(text_rows, cols);
    solar_os_tui_clear(&editor.tui);
    (void)solar_os_tui_set_cursor_visible(&editor.tui, false);

    snprintf(header,
             sizeof(header),
             "edit %s%s",
             editor.display_name,
             editor.dirty ? " *" : "");
    solar_os_tui_write_cell(&editor.tui, 0, 0, cols, header,
                            SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);
    editor_prepare_syntax_state(&syntax_state, editor.top_line);

    for (size_t row = 0; row < text_rows; row++) {
        const size_t line_index = editor.top_line + row;
        const size_t start = editor_index_for_line(line_index);
        const size_t end = editor_line_end_for(start);
        char line[SOLAR_OS_TERMINAL_MAX_COLS];
        uint8_t styles[SOLAR_OS_TERMINAL_MAX_COLS];
        size_t line_len = 0;
        size_t visible_start = start + editor.left_col;
        size_t copy_len = 0;

        if (start < editor.len || line_index == 0) {
            line_len = end >= start ? end - start : 0;
            if (editor.left_col < line_len) {
                copy_len = line_len - editor.left_col;
                if (copy_len > cols) {
                    copy_len = cols;
                }
                if (copy_len > SOLAR_OS_TERMINAL_MAX_COLS) {
                    copy_len = SOLAR_OS_TERMINAL_MAX_COLS;
                }
                visible_start = start + editor.left_col;
                memcpy(line, &editor.buffer[visible_start], copy_len);
            }
        }
        if (editor.syntax != SOLAR_OS_SYNTAX_NONE && (start < editor.len || line_index == 0)) {
            solar_os_syntax_highlight_line(editor.syntax,
                                           &syntax_state,
                                           &editor.buffer[start],
                                           line_len,
                                           editor.left_col,
                                           styles,
                                           copy_len);
        } else if (copy_len > 0) {
            memset(styles, SOLAR_OS_SYNTAX_STYLE_NORMAL, copy_len);
        }

        for (size_t col = 0; col < copy_len; col++) {
            const size_t index = visible_start + col;
            const bool selected = has_selection && index >= selection_start && index < selection_end;
            const solar_os_syntax_style_t style = (solar_os_syntax_style_t)styles[col];
            const uint32_t codepoint = editor_is_printable(line[col]) ?
                (uint8_t)line[col] :
                (uint32_t)'.';
            (void)solar_os_tui_putch(&editor.tui,
                                     row + 1,
                                     col,
                                     codepoint,
                                     editor_tui_attr(style, selected));
        }
    }

    if (rows > 1) {
        char footer[192];
        if (editor.search.input_active) {
            snprintf(footer,
                     sizeof(footer),
                     "Find: %s_",
                     editor.search.input);
        } else if (editor.message[0] != '\0') {
            snprintf(footer,
                     sizeof(footer),
                     "Ln %u Col %u  %s",
                     (unsigned)(cursor_line + 1),
                     (unsigned)(cursor_col + 1),
                     editor.message);
        } else {
            snprintf(footer,
                     sizeof(footer),
                     "Ln %u Col %u  %u/%u bytes  ^S save  ^Q/ESC quit",
                     (unsigned)(cursor_line + 1),
                     (unsigned)(cursor_col + 1),
                     (unsigned)editor.len,
                     (unsigned)(editor.capacity - 1U));
        }
        solar_os_tui_draw_help(&editor.tui, footer);
    }

    if (text_rows > 0 &&
        cursor_line >= editor.top_line &&
        cursor_line < editor.top_line + text_rows &&
        cursor_col >= editor.left_col) {
        const size_t screen_col = cursor_col - editor.left_col;
        if (screen_col < cols) {
            (void)solar_os_tui_move(&editor.tui,
                                    cursor_line - editor.top_line + 1,
                                    screen_col);
            (void)solar_os_tui_set_cursor_visible(&editor.tui, true);
        }
    }
    solar_os_tui_refresh(&editor.tui);
}

static bool editor_delete_range(size_t start, size_t end)
{
    if (start >= end || start >= editor.len) {
        return false;
    }
    if (end > editor.len) {
        end = editor.len;
    }

    memmove(&editor.buffer[start], &editor.buffer[end], editor.len - end);
    editor.len -= end - start;
    editor.cursor = start;
    editor.buffer[editor.len] = '\0';
    editor.dirty = true;
    editor_update_cursor_column();
    editor_clear_selection();
    editor_set_message("");
    return true;
}

static bool editor_delete_selection(void)
{
    if (!editor_has_selection()) {
        return false;
    }

    size_t start;
    size_t end;
    editor_selection_bounds(&start, &end);
    return editor_delete_range(start, end);
}

static bool editor_insert_char(char ch)
{
    size_t selection_start = 0;
    size_t selection_end = 0;
    const bool replacing = editor_has_selection();
    if (replacing) {
        editor_selection_bounds(&selection_start, &selection_end);
    }

    const size_t selection_len = replacing ? selection_end - selection_start : 0;
    if (editor.len - selection_len + 1 >= editor.capacity) {
        editor_set_capacity_message("buffer full");
        return false;
    }

    if (replacing) {
        editor_delete_range(selection_start, selection_end);
    }

    memmove(&editor.buffer[editor.cursor + 1],
            &editor.buffer[editor.cursor],
            editor.len - editor.cursor);
    editor.buffer[editor.cursor] = ch;
    editor.cursor++;
    editor.len++;
    editor.buffer[editor.len] = '\0';
    editor.dirty = true;
    editor_update_cursor_column();
    editor_set_message("");
    return true;
}

static void editor_backspace(void)
{
    if (editor_delete_selection()) {
        return;
    }

    if (editor.cursor == 0) {
        return;
    }

    memmove(&editor.buffer[editor.cursor - 1],
            &editor.buffer[editor.cursor],
            editor.len - editor.cursor);
    editor.cursor--;
    editor.len--;
    editor.buffer[editor.len] = '\0';
    editor.dirty = true;
    editor_update_cursor_column();
    editor_set_message("");
}

static void editor_delete_forward(void)
{
    if (editor_delete_selection()) {
        return;
    }
    if (editor.cursor >= editor.len) {
        return;
    }

    editor_delete_range(editor.cursor, editor.cursor + 1);
}

static bool editor_hex_append_byte(void)
{
    if (editor.len + 1U >= editor.capacity) {
        editor_set_capacity_message("buffer full");
        return false;
    }
    editor.buffer[editor.len++] = '\0';
    editor.buffer[editor.len] = '\0';
    return true;
}

static bool editor_hex_insert_byte(uint8_t value)
{
    if (editor.len + 1U >= editor.capacity || editor.cursor > editor.len) {
        editor_set_capacity_message("buffer full");
        return false;
    }
    memmove(&editor.buffer[editor.cursor + 1U],
            &editor.buffer[editor.cursor],
            editor.len - editor.cursor);
    editor.buffer[editor.cursor] = (char)value;
    editor.len++;
    editor.buffer[editor.len] = '\0';
    return true;
}

static int editor_hex_digit_value(char ch)
{
    const unsigned char value = (unsigned char)ch;
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static void editor_hex_write_nibble(uint8_t nibble)
{
    const bool replacing = editor_has_selection();
    if (replacing) {
        (void)editor_delete_selection();
        editor.hex_nibble = 0;
    }
    if (replacing && !editor_hex_insert_byte(0)) {
        return;
    }
    if (!replacing && editor.cursor == editor.len && !editor_hex_append_byte()) {
        return;
    }
    if (editor.cursor >= editor.len) {
        return;
    }

    const size_t index = editor.cursor;
    uint8_t value = (uint8_t)editor.buffer[index];
    if (editor.hex_nibble == 0) {
        value = (uint8_t)((value & 0x0fU) | (nibble << 4U));
        editor.hex_nibble = 1;
    } else {
        value = (uint8_t)((value & 0xf0U) | nibble);
        editor.hex_nibble = 0;
        editor.cursor++;
    }
    editor.buffer[index] = (char)value;
    editor.dirty = true;
    editor_clear_selection();
    editor_set_message("");
}

static void editor_hex_write_byte(uint8_t value)
{
    const bool replacing = editor_has_selection();
    if (replacing) {
        (void)editor_delete_selection();
    }
    if (replacing && !editor_hex_insert_byte(value)) {
        return;
    }
    if (!replacing && editor.cursor == editor.len && !editor_hex_append_byte()) {
        return;
    }
    if (editor.cursor >= editor.len) {
        return;
    }

    if (!replacing) {
        editor.buffer[editor.cursor] = (char)value;
    }
    editor.cursor++;
    editor.buffer[editor.len] = '\0';
    editor.hex_nibble = 0;
    editor.dirty = true;
    editor_clear_selection();
    editor_set_message("");
}

static void editor_move_left(void)
{
    if (editor.cursor > 0) {
        editor.cursor--;
        editor_update_preferred_col();
    }
}

static void editor_move_right(void)
{
    if (editor.cursor < editor.len) {
        editor.cursor++;
        editor_update_preferred_col();
    }
}

static void editor_move_home(void)
{
    editor.cursor = editor_line_start_for(editor.cursor);
    editor_update_cursor_column();
}

static void editor_move_end(void)
{
    editor.cursor = editor_line_end_for(editor_line_start_for(editor.cursor));
    editor_update_preferred_col();
}

static void editor_move_document_start(void)
{
    editor.cursor = 0;
    editor_update_preferred_col();
}

static void editor_move_document_end(void)
{
    editor.cursor = editor.len;
    editor_update_preferred_col();
}

static bool editor_is_word_char(char ch)
{
    const unsigned char value = (unsigned char)ch;

    return isalnum(value) || value >= 0xa0 || ch == '_';
}

static void editor_move_word_left(void)
{
    size_t cursor = editor.cursor;

    while (cursor > 0 && !editor_is_word_char(editor.buffer[cursor - 1])) {
        cursor--;
    }
    while (cursor > 0 && editor_is_word_char(editor.buffer[cursor - 1])) {
        cursor--;
    }

    editor.cursor = cursor;
    editor_update_preferred_col();
}

static void editor_move_word_right(void)
{
    size_t cursor = editor.cursor;

    while (cursor < editor.len && editor_is_word_char(editor.buffer[cursor])) {
        cursor++;
    }
    while (cursor < editor.len && !editor_is_word_char(editor.buffer[cursor])) {
        cursor++;
    }

    editor.cursor = cursor;
    editor_update_preferred_col();
}

static void editor_move_up(void)
{
    const size_t start = editor_line_start_for(editor.cursor);
    if (start == 0) {
        return;
    }

    const size_t previous_end = start - 1;
    const size_t previous_start = editor_line_start_for(previous_end);
    const size_t previous_len = previous_end - previous_start;
    const size_t col = editor.preferred_col < previous_len ? editor.preferred_col : previous_len;
    editor.cursor = previous_start + col;
}

static void editor_move_down(void)
{
    const size_t start = editor_line_start_for(editor.cursor);
    const size_t end = editor_line_end_for(start);
    if (end >= editor.len) {
        return;
    }

    const size_t next_start = end + 1;
    const size_t next_end = editor_line_end_for(next_start);
    const size_t next_len = next_end - next_start;
    const size_t col = editor.preferred_col < next_len ? editor.preferred_col : next_len;
    editor.cursor = next_start + col;
}

static void editor_page_up(void)
{
    const size_t rows = solar_os_tui_rows(&editor.tui);
    const size_t page = rows > 2 ? rows - 2 : 1;

    for (size_t i = 0; i < page; i++) {
        editor_move_up();
    }
}

static void editor_page_down(void)
{
    const size_t rows = solar_os_tui_rows(&editor.tui);
    const size_t page = rows > 2 ? rows - 2 : 1;

    for (size_t i = 0; i < page; i++) {
        editor_move_down();
    }
}

static size_t editor_hex_current_bytes_per_row(void)
{
    return editor_hex_bytes_per_row(solar_os_tui_cols(&editor.tui));
}

static void editor_hex_set_cursor(size_t cursor)
{
    editor.cursor = cursor <= editor.len ? cursor : editor.len;
    editor.hex_nibble = 0;
}

static void editor_hex_move_left(void)
{
    if (editor.cursor > 0) {
        editor_hex_set_cursor(editor.cursor - 1U);
    }
}

static void editor_hex_move_right(void)
{
    if (editor.cursor < editor.len) {
        editor_hex_set_cursor(editor.cursor + 1U);
    }
}

static void editor_hex_move_up(void)
{
    const size_t bytes_per_row = editor_hex_current_bytes_per_row();
    if (editor.cursor >= bytes_per_row) {
        editor_hex_set_cursor(editor.cursor - bytes_per_row);
    }
}

static void editor_hex_move_down(void)
{
    const size_t bytes_per_row = editor_hex_current_bytes_per_row();
    const size_t row = editor.cursor / bytes_per_row;
    const size_t last_row = editor.len / bytes_per_row;
    if (row < last_row) {
        const size_t target = editor.cursor + bytes_per_row;
        editor_hex_set_cursor(target < editor.len ? target : editor.len);
    }
}

static void editor_hex_move_home(void)
{
    const size_t bytes_per_row = editor_hex_current_bytes_per_row();
    editor_hex_set_cursor((editor.cursor / bytes_per_row) * bytes_per_row);
}

static void editor_hex_move_end(void)
{
    const size_t bytes_per_row = editor_hex_current_bytes_per_row();
    const size_t row_start = (editor.cursor / bytes_per_row) * bytes_per_row;
    size_t target = row_start + bytes_per_row - 1U;
    if (target > editor.len) {
        target = editor.len;
    }
    editor_hex_set_cursor(target);
}

static void editor_hex_move_document_start(void)
{
    editor_hex_set_cursor(0);
}

static void editor_hex_move_document_end(void)
{
    editor_hex_set_cursor(editor.len);
}

static void editor_hex_page(bool down)
{
    const size_t rows = solar_os_tui_rows(&editor.tui);
    const size_t visible_rows = rows > 2U ? rows - 2U : 1U;
    const size_t distance = visible_rows * editor_hex_current_bytes_per_row();
    if (down) {
        const size_t remaining = editor.len - editor.cursor;
        editor_hex_set_cursor(editor.cursor + (distance < remaining ? distance : remaining));
    } else {
        editor_hex_set_cursor(editor.cursor > distance ? editor.cursor - distance : 0U);
    }
}

static esp_err_t editor_copy_selection_to_clipboard(size_t *copied_len)
{
    if (!editor_has_selection()) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t start;
    size_t end;
    editor_selection_bounds(&start, &end);
    if (copied_len != NULL) {
        *copied_len = end - start;
    }
    return solar_os_clipboard_set(&editor.buffer[start], end - start);
}

static void editor_copy_selection(void)
{
    size_t copied = 0;
    const esp_err_t err = editor_copy_selection_to_clipboard(&copied);

    if (err == ESP_ERR_NOT_FOUND) {
        editor_set_message("no selection");
    } else if (err == ESP_ERR_INVALID_SIZE) {
        editor_set_message("selection too large");
    } else if (err != ESP_OK) {
        editor_set_message("copy failed");
    } else {
        char message[sizeof(editor.message)];
        snprintf(message, sizeof(message), "copied %u bytes", (unsigned)copied);
        editor_set_message(message);
    }
}

static void editor_cut_selection(void)
{
    size_t copied = 0;
    const esp_err_t err = editor_copy_selection_to_clipboard(&copied);

    if (err == ESP_ERR_NOT_FOUND) {
        editor_set_message("no selection");
    } else if (err == ESP_ERR_INVALID_SIZE) {
        editor_set_message("selection too large");
    } else if (err != ESP_OK) {
        editor_set_message("cut failed");
    } else if (editor_delete_selection()) {
        char message[sizeof(editor.message)];
        snprintf(message, sizeof(message), "cut %u bytes", (unsigned)copied);
        editor_set_message(message);
    }
}

static void editor_paste_clipboard(void)
{
    size_t paste_len = 0;
    const char *paste = solar_os_clipboard_data(&paste_len);
    if (paste == NULL || paste_len == 0) {
        editor_set_message("clipboard empty");
        return;
    }

    size_t selection_start = 0;
    size_t selection_end = 0;
    const bool replacing = editor_has_selection();
    if (replacing) {
        editor_selection_bounds(&selection_start, &selection_end);
    }

    const size_t selection_len = replacing ? selection_end - selection_start : 0;
    if (editor.len - selection_len + paste_len >= editor.capacity) {
        editor_set_capacity_message("buffer full");
        return;
    }

    if (replacing) {
        editor_delete_range(selection_start, selection_end);
    }

    memmove(&editor.buffer[editor.cursor + paste_len],
            &editor.buffer[editor.cursor],
            editor.len - editor.cursor);
    memcpy(&editor.buffer[editor.cursor], paste, paste_len);
    editor.cursor += paste_len;
    editor.len += paste_len;
    editor.buffer[editor.len] = '\0';
    editor.dirty = true;
    editor_update_cursor_column();
    editor_clear_selection();

    char message[sizeof(editor.message)];
    snprintf(message, sizeof(message), "pasted %u bytes", (unsigned)paste_len);
    editor_set_message(message);
}

static void editor_select_all(void)
{
    if (editor.len == 0) {
        editor_clear_selection();
        editor_set_message("empty buffer");
        return;
    }

    editor.selection_anchor = 0;
    editor.cursor = editor.len;
    editor.selection_active = true;
    editor_update_cursor_column();
    editor_set_message("selected all");
}

static esp_err_t editor_save(void)
{
    FILE *file = fopen(editor.path, "wb");
    if (file == NULL) {
        char message[sizeof(editor.message)];
        snprintf(message, sizeof(message), "save failed: %s", strerror(errno));
        editor_set_message(message);
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    if (editor.len > 0 && fwrite(editor.buffer, 1, editor.len, file) != editor.len) {
        ret = ESP_FAIL;
    }

    const int write_errno = errno;
    if (fclose(file) != 0 && ret == ESP_OK) {
        ret = ESP_FAIL;
    }

    if (ret != ESP_OK) {
        char message[sizeof(editor.message)];
        const int error_number = write_errno != 0 ? write_errno : EIO;
        snprintf(message, sizeof(message), "save failed: %s", strerror(error_number));
        editor_set_message(message);
        return ret;
    }

    editor.dirty = false;
    editor_set_message("saved");
    return ESP_OK;
}

static void editor_open_empty(void)
{
    editor.len = 0;
    editor.cursor = 0;
    editor.preferred_col = 0;
    editor.top_line = 0;
    editor.left_col = 0;
    editor.selection_anchor = 0;
    editor.selection_active = false;
    editor.hex_pane = EDITOR_HEX_PANE_HEX;
    editor.hex_nibble = 0;
    editor.dirty = false;
    editor.buffer[0] = '\0';
    editor_set_message("");
}

static esp_err_t editor_open_file(void)
{
    FILE *file = fopen(editor.path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            editor_open_empty();
            return ESP_OK;
        }

        char message[sizeof(editor.message)];
        snprintf(message, sizeof(message), "open failed: %s", strerror(errno));
        editor_set_message(message);
        return ESP_FAIL;
    }

    editor.len = fread(editor.buffer, 1, editor.capacity - 1, file);
    if (ferror(file)) {
        char message[sizeof(editor.message)];
        snprintf(message, sizeof(message), "read failed: %s", strerror(errno));
        fclose(file);
        editor_set_message(message);
        return ESP_FAIL;
    }

    const int extra = fgetc(file);
    fclose(file);
    if (extra != EOF) {
        editor.len = 0;
        editor.buffer[0] = '\0';
        editor_set_capacity_message("file too large");
        return ESP_ERR_INVALID_SIZE;
    }

    editor.buffer[editor.len] = '\0';
    editor.cursor = 0;
    editor.preferred_col = 0;
    editor.top_line = 0;
    editor.left_col = 0;
    editor.selection_anchor = 0;
    editor.selection_active = false;
    editor.hex_pane = EDITOR_HEX_PANE_HEX;
    editor.hex_nibble = 0;
    editor.dirty = false;
    editor_set_message("");
    return ESP_OK;
}

static esp_err_t edit_start(solar_os_context_t *ctx)
{
    memset(&editor, 0, sizeof(editor));

    const int argc = solar_os_context_argc(ctx);
    const char *command = solar_os_context_argv(ctx, 0);
    editor.mode = command != NULL && strcmp(command, "hexedit") == 0 ?
        EDITOR_MODE_HEX :
        EDITOR_MODE_TEXT;

    const bool has_psram = solar_os_board_has(SOLAR_OS_BOARD_CAP_PSRAM);
    editor.capacity = has_psram ?
        EDITOR_PSRAM_BUFFER_CAPACITY :
        EDITOR_INTERNAL_BUFFER_CAPACITY;
    editor.buffer = solar_os_memory_alloc(editor.capacity,
                                           has_psram ?
                                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED :
                                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                           "edit.buffer");
    if (editor.buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t tui_err = solar_os_tui_screen_begin(&editor.tui, ctx);
    if (tui_err != ESP_OK) {
        solar_os_memory_free(editor.buffer);
        memset(&editor, 0, sizeof(editor));
        return tui_err;
    }
    editor_capture_text_size();

    if (argc != 2) {
        char usage[sizeof(editor.message)];
        snprintf(usage, sizeof(usage), "usage: %s <file>", editor_app_name());
        solar_os_context_finish(ctx, 2, usage);
        return ESP_OK;
    }

    if (!solar_os_storage_is_mounted()) {
        solar_os_context_finish(ctx, 1, "edit: storage not mounted");
        return ESP_OK;
    }

    const char *arg = solar_os_context_argv(ctx, 1);
    const esp_err_t path_err = solar_os_storage_resolve_path(arg,
                                                             editor.path,
                                                             sizeof(editor.path));
    if (path_err != ESP_OK) {
        solar_os_context_finish(
            ctx,
            1,
            path_err == ESP_ERR_INVALID_SIZE ?
                "edit: path too long" : "edit: invalid path");
        return ESP_OK;
    }
    strlcpy(editor.display_name, arg != NULL ? arg : editor.path, sizeof(editor.display_name));
    editor.syntax = editor.mode == EDITOR_MODE_TEXT ?
        solar_os_syntax_language_for_path(editor.path) :
        SOLAR_OS_SYNTAX_NONE;

    const esp_err_t err = editor_open_file();
    if (err != ESP_OK) {
        char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
        snprintf(message, sizeof(message), "%s: %s", editor_app_name(), editor.message);
        solar_os_context_finish(ctx, 1, message);
        return ESP_OK;
    }

    editor_render(ctx);
    return ESP_OK;
}

static void edit_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    editor_restore_text_size();
    (void)solar_os_tui_set_cursor_visible(&editor.tui, true);
    solar_os_tui_refresh(&editor.tui);
    solar_os_tui_end(&editor.tui);
    solar_os_memory_free(editor.buffer);
    memset(&editor, 0, sizeof(editor));
}

static void edit_resume(solar_os_context_t *ctx)
{
    editor_render(ctx);
}

static void edit_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    if (editor.display_name[0] != '\0') {
        snprintf(buffer,
                 buffer_len,
                 "%s %s%s",
                 editor_app_name(),
                 editor.display_name,
                 editor.dirty ? "*" : "");
        return;
    }
    strlcpy(buffer, editor_app_name(), buffer_len);
}

static void editor_apply_move(bool selecting, void (*move)(void))
{
    editor_begin_selection(selecting);
    move();
    editor_finish_selection(selecting);
}

static void editor_apply_page_move(bool selecting, bool down)
{
    editor_begin_selection(selecting);
    if (down) {
        editor_page_down();
    } else {
        editor_page_up();
    }
    editor_finish_selection(selecting);
}

static void editor_apply_hex_page_move(bool selecting, bool down)
{
    editor_begin_selection(selecting);
    editor_hex_page(down);
    editor_finish_selection(selecting);
}

static bool editor_find_next(void)
{
    if (editor.search.query_len == 0U) {
        editor_set_message("no search");
        return false;
    }
    solar_os_text_search_match_t match;
    if (!solar_os_text_search_find(editor.buffer,
                                   editor.len,
                                   editor.search.query,
                                   editor.cursor,
                                   false,
                                   SOLAR_OS_TEXT_SEARCH_FORWARD,
                                   &match)) {
        editor.search.match_valid = false;
        editor_set_message("not found");
        return false;
    }
    editor.search.match = match;
    editor.search.match_valid = true;
    editor.selection_anchor = match.offset;
    editor.cursor = match.offset + match.length;
    editor.selection_active = match.length > 0U;
    editor_update_preferred_col();
    editor_set_message(match.wrapped ? "found (wrapped)" : "found");
    return true;
}

static bool editor_handle_search_input(solar_os_context_t *ctx, uint8_t key)
{
    switch (key) {
    case SOLAR_OS_KEY_ESCAPE:
        solar_os_text_search_cancel_input(&editor.search);
        break;
    case '\r':
    case '\n':
        if (solar_os_text_search_submit_input(&editor.search)) {
            (void)editor_find_next();
        } else {
            editor_set_message("empty search");
        }
        break;
    case '\b':
    case 0x7fU:
        (void)solar_os_text_search_input_backspace(&editor.search);
        break;
    default:
        if (editor_is_printable((char)key)) {
            (void)solar_os_text_search_input_append(&editor.search, (char)key);
        }
        break;
    }
    editor_render(ctx);
    return true;
}

static bool editor_hex_event(solar_os_context_t *ctx, uint8_t key)
{
    switch (key) {
    case SOLAR_OS_KEY_ESCAPE:
    case 0x11:
        solar_os_context_finish(ctx, 0, NULL);
        break;
    case 0x01:
        editor_select_all();
        break;
    case 0x03:
        editor_copy_selection();
        break;
    case 0x13:
        (void)editor_save();
        break;
    case 0x16:
        editor_paste_clipboard();
        break;
    case 0x18:
        editor_cut_selection();
        break;
    case SOLAR_OS_KEY_CTRL_PLUS:
        editor_adjust_text_size(1);
        break;
    case SOLAR_OS_KEY_CTRL_MINUS:
        editor_adjust_text_size(-1);
        break;
    case '\t':
        editor.hex_pane = editor.hex_pane == EDITOR_HEX_PANE_HEX ?
            EDITOR_HEX_PANE_ASCII :
            EDITOR_HEX_PANE_HEX;
        editor.hex_nibble = 0;
        editor_set_message("");
        break;
    case SOLAR_OS_KEY_LEFT:
    case SOLAR_OS_KEY_CTRL_LEFT:
        editor_apply_move(false, editor_hex_move_left);
        break;
    case SOLAR_OS_KEY_SHIFT_LEFT:
    case SOLAR_OS_KEY_CTRL_SHIFT_LEFT:
        editor_apply_move(true, editor_hex_move_left);
        break;
    case SOLAR_OS_KEY_RIGHT:
    case SOLAR_OS_KEY_CTRL_RIGHT:
        editor_apply_move(false, editor_hex_move_right);
        break;
    case SOLAR_OS_KEY_SHIFT_RIGHT:
    case SOLAR_OS_KEY_CTRL_SHIFT_RIGHT:
        editor_apply_move(true, editor_hex_move_right);
        break;
    case SOLAR_OS_KEY_UP:
    case SOLAR_OS_KEY_CTRL_UP:
        editor_apply_move(false, editor_hex_move_up);
        break;
    case SOLAR_OS_KEY_SHIFT_UP:
    case SOLAR_OS_KEY_CTRL_SHIFT_UP:
        editor_apply_move(true, editor_hex_move_up);
        break;
    case SOLAR_OS_KEY_DOWN:
    case SOLAR_OS_KEY_CTRL_DOWN:
        editor_apply_move(false, editor_hex_move_down);
        break;
    case SOLAR_OS_KEY_SHIFT_DOWN:
    case SOLAR_OS_KEY_CTRL_SHIFT_DOWN:
        editor_apply_move(true, editor_hex_move_down);
        break;
    case SOLAR_OS_KEY_HOME:
        editor_apply_move(false, editor_hex_move_home);
        break;
    case SOLAR_OS_KEY_SHIFT_HOME:
        editor_apply_move(true, editor_hex_move_home);
        break;
    case SOLAR_OS_KEY_CTRL_HOME:
        editor_apply_move(false, editor_hex_move_document_start);
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_HOME:
        editor_apply_move(true, editor_hex_move_document_start);
        break;
    case SOLAR_OS_KEY_END:
        editor_apply_move(false, editor_hex_move_end);
        break;
    case SOLAR_OS_KEY_SHIFT_END:
        editor_apply_move(true, editor_hex_move_end);
        break;
    case SOLAR_OS_KEY_CTRL_END:
        editor_apply_move(false, editor_hex_move_document_end);
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_END:
        editor_apply_move(true, editor_hex_move_document_end);
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        editor_apply_hex_page_move(false, false);
        break;
    case SOLAR_OS_KEY_SHIFT_PAGE_UP:
        editor_apply_hex_page_move(true, false);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        editor_apply_hex_page_move(false, true);
        break;
    case SOLAR_OS_KEY_SHIFT_PAGE_DOWN:
        editor_apply_hex_page_move(true, true);
        break;
    case SOLAR_OS_KEY_DELETE:
        editor_delete_forward();
        break;
    case '\b':
        editor_backspace();
        break;
    case '\r':
    case '\n':
        if (editor.hex_pane == EDITOR_HEX_PANE_ASCII) {
            editor_hex_write_byte('\n');
        }
        break;
    default:
        if (editor.hex_pane == EDITOR_HEX_PANE_HEX) {
            const int digit = editor_hex_digit_value((char)key);
            if (digit >= 0) {
                editor_hex_write_nibble((uint8_t)digit);
            }
        } else if (editor_is_printable((char)key)) {
            editor_hex_write_byte(key);
        }
        break;
    }

    editor_render(ctx);
    return true;
}

static bool edit_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL || event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const char ch = event->data.ch;
    if ((uint8_t)ch == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }

    if (editor.mode == EDITOR_MODE_HEX) {
        return editor_hex_event(ctx, (uint8_t)ch);
    }

    if (editor.search.input_active) {
        return editor_handle_search_input(ctx, (uint8_t)ch);
    }

    switch ((uint8_t)ch) {
    case SOLAR_OS_KEY_ESCAPE:
    case 0x11:
        solar_os_context_finish(ctx, 0, NULL);
        break;
    case 0x01:
        editor_select_all();
        break;
    case 0x03:
        editor_copy_selection();
        break;
    case 0x06:
        solar_os_text_search_begin_input(&editor.search);
        editor_set_message("");
        break;
    case 0x13:
        (void)editor_save();
        break;
    case 0x16:
        editor_paste_clipboard();
        break;
    case 0x18:
        editor_cut_selection();
        break;
    case SOLAR_OS_KEY_CTRL_PLUS:
        editor_adjust_text_size(1);
        break;
    case SOLAR_OS_KEY_CTRL_MINUS:
        editor_adjust_text_size(-1);
        break;
    case SOLAR_OS_KEY_F3:
        (void)editor_find_next();
        break;
    case SOLAR_OS_KEY_LEFT:
        editor_apply_move(false, editor_move_left);
        break;
    case SOLAR_OS_KEY_SHIFT_LEFT:
        editor_apply_move(true, editor_move_left);
        break;
    case SOLAR_OS_KEY_CTRL_LEFT:
        editor_apply_move(false, editor_move_word_left);
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_LEFT:
        editor_apply_move(true, editor_move_word_left);
        break;
    case SOLAR_OS_KEY_RIGHT:
        editor_apply_move(false, editor_move_right);
        break;
    case SOLAR_OS_KEY_SHIFT_RIGHT:
        editor_apply_move(true, editor_move_right);
        break;
    case SOLAR_OS_KEY_CTRL_RIGHT:
        editor_apply_move(false, editor_move_word_right);
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_RIGHT:
        editor_apply_move(true, editor_move_word_right);
        break;
    case SOLAR_OS_KEY_UP:
    case SOLAR_OS_KEY_CTRL_UP:
        editor_apply_move(false, editor_move_up);
        break;
    case SOLAR_OS_KEY_SHIFT_UP:
    case SOLAR_OS_KEY_CTRL_SHIFT_UP:
        editor_apply_move(true, editor_move_up);
        break;
    case SOLAR_OS_KEY_DOWN:
    case SOLAR_OS_KEY_CTRL_DOWN:
        editor_apply_move(false, editor_move_down);
        break;
    case SOLAR_OS_KEY_SHIFT_DOWN:
    case SOLAR_OS_KEY_CTRL_SHIFT_DOWN:
        editor_apply_move(true, editor_move_down);
        break;
    case SOLAR_OS_KEY_HOME:
        editor_apply_move(false, editor_move_home);
        break;
    case SOLAR_OS_KEY_SHIFT_HOME:
        editor_apply_move(true, editor_move_home);
        break;
    case SOLAR_OS_KEY_CTRL_HOME:
        editor_apply_move(false, editor_move_document_start);
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_HOME:
        editor_apply_move(true, editor_move_document_start);
        break;
    case SOLAR_OS_KEY_END:
        editor_apply_move(false, editor_move_end);
        break;
    case SOLAR_OS_KEY_SHIFT_END:
        editor_apply_move(true, editor_move_end);
        break;
    case SOLAR_OS_KEY_CTRL_END:
        editor_apply_move(false, editor_move_document_end);
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_END:
        editor_apply_move(true, editor_move_document_end);
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        editor_apply_page_move(false, false);
        break;
    case SOLAR_OS_KEY_SHIFT_PAGE_UP:
        editor_apply_page_move(true, false);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        editor_apply_page_move(false, true);
        break;
    case SOLAR_OS_KEY_SHIFT_PAGE_DOWN:
        editor_apply_page_move(true, true);
        break;
    case SOLAR_OS_KEY_DELETE:
        editor_delete_forward();
        break;
    case '\b':
        editor_backspace();
        break;
    case '\r':
    case '\n':
        editor_insert_char('\n');
        break;
    case '\t':
        do {
            if (!editor_insert_char(' ')) {
                break;
            }
        } while ((editor_cursor_col() % EDITOR_TAB_WIDTH) != 0);
        break;
    default:
        if (editor_is_printable(ch)) {
            editor_insert_char(ch);
        }
        break;
    }

    editor_render(ctx);
    return true;
}

const solar_os_app_t solar_os_edit_app = {
    .name = "edit",
    .summary = "text editor",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = edit_start,
    .resume = edit_resume,
    .stop = edit_stop,
    .event = edit_event,
    .title = edit_title,
    .state_slot = &editor_state,
    .state_size = sizeof(editor_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
};

const solar_os_app_t solar_os_hexedit_app = {
    .name = "hexedit",
    .summary = "two-pane hex editor",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = edit_start,
    .resume = edit_resume,
    .stop = edit_stop,
    .event = edit_event,
    .title = edit_title,
    .state_slot = &editor_state,
    .state_size = sizeof(editor_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
};
