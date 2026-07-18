#include "solar_os_edit.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_clipboard.h"
#include "solar_os_memory.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"
#include "solar_os_syntax.h"
#include "solar_os_terminal.h"

#define EDITOR_BUFFER_CAPACITY (256 * 1024)
#define EDITOR_TAB_WIDTH 4

typedef struct {
    char *buffer;
    size_t len;
    size_t capacity;
    size_t cursor;
    size_t preferred_col;
    size_t top_line;
    size_t left_col;
    size_t selection_anchor;
    bool dirty;
    bool error_only;
    bool selection_active;
    bool saved_text_size_valid;
    solar_os_terminal_text_size_t saved_text_size;
    solar_os_syntax_language_t syntax;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char display_name[SOLAR_OS_STORAGE_PATH_MAX];
    char message[72];
} editor_state_t;

typedef struct {
    bool bold;
    bool italic;
    bool underline;
    bool inverse;
} editor_attrs_t;

static editor_state_t editor;
static solar_os_shell_io_t editor_fallback_io;

static solar_os_shell_io_t *editor_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&editor_fallback_io, solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &editor_fallback_io);
        io = &editor_fallback_io;
    }
    return io;
}

static const solar_os_terminal_text_size_t editor_text_sizes[] = {
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

static void editor_set_message(const char *message)
{
    strlcpy(editor.message, message != NULL ? message : "", sizeof(editor.message));
}

static void editor_capture_text_size(solar_os_context_t *ctx)
{
    solar_os_terminal_t *terminal = solar_os_shell_io_terminal(editor_io(ctx));
    if (terminal == NULL) {
        return;
    }

    editor.saved_text_size = solar_os_terminal_text_size(terminal);
    editor.saved_text_size_valid = true;
}

static void editor_restore_text_size(solar_os_context_t *ctx)
{
    if (!editor.saved_text_size_valid) {
        return;
    }

    solar_os_terminal_t *terminal = solar_os_shell_io_terminal(editor_io(ctx));
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

static void editor_adjust_text_size(solar_os_context_t *ctx, int delta)
{
    solar_os_terminal_t *terminal = solar_os_shell_io_terminal(editor_io(ctx));
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

static void editor_write_clipped(solar_os_shell_io_t *io,
                                 const char *text,
                                 size_t max_cols,
                                 bool bold)
{
    char clipped[SOLAR_OS_TERMINAL_MAX_COLS + 1];
    const size_t limit = max_cols < SOLAR_OS_TERMINAL_MAX_COLS ?
        max_cols :
        SOLAR_OS_TERMINAL_MAX_COLS;

    strlcpy(clipped, text != NULL ? text : "", sizeof(clipped));
    clipped[limit] = '\0';
    if (bold) {
        solar_os_shell_io_write_bold(io, clipped);
    } else {
        solar_os_shell_io_write(io, clipped);
    }
}

static void editor_apply_style(solar_os_shell_io_t *io,
                               editor_attrs_t *attrs,
                               solar_os_syntax_style_t style,
                               bool inverse)
{
    const bool bold = style == SOLAR_OS_SYNTAX_STYLE_KEYWORD;
    const bool underline = style == SOLAR_OS_SYNTAX_STYLE_COMMENT;
    const bool italic = style == SOLAR_OS_SYNTAX_STYLE_STRING ||
        style == SOLAR_OS_SYNTAX_STYLE_NUMBER;

    if (attrs == NULL || attrs->bold != bold) {
        (void)solar_os_shell_io_set_bold(io, bold);
        if (attrs != NULL) {
            attrs->bold = bold;
        }
    }
    if (attrs == NULL || attrs->underline != underline) {
        (void)solar_os_shell_io_set_underline(io, underline);
        if (attrs != NULL) {
            attrs->underline = underline;
        }
    }
    if (attrs == NULL || attrs->italic != italic) {
        (void)solar_os_shell_io_set_italic(io, italic);
        if (attrs != NULL) {
            attrs->italic = italic;
        }
    }
    if (attrs == NULL || attrs->inverse != inverse) {
        (void)solar_os_shell_io_set_inverse(io, inverse);
        if (attrs != NULL) {
            attrs->inverse = inverse;
        }
    }
}

static void editor_reset_style(solar_os_shell_io_t *io)
{
    (void)solar_os_shell_io_set_bold(io, false);
    (void)solar_os_shell_io_set_underline(io, false);
    (void)solar_os_shell_io_set_italic(io, false);
    (void)solar_os_shell_io_set_inverse(io, false);
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

static void editor_ensure_cursor_visible(solar_os_shell_io_t *io)
{
    const size_t rows = solar_os_shell_io_rows(io);
    const size_t cols = solar_os_shell_io_cols(io);
    const size_t text_rows = rows > 1 ? rows - 1 : 1;
    const size_t cursor_line = editor_line_for_index(editor.cursor);
    const size_t cursor_col = editor_cursor_col();

    if (cursor_line < editor.top_line) {
        editor.top_line = cursor_line;
    } else if (cursor_line >= editor.top_line + text_rows) {
        editor.top_line = cursor_line - text_rows + 1;
    }

    if (cursor_col < editor.left_col) {
        editor.left_col = cursor_col;
    } else if (cursor_col >= editor.left_col + cols) {
        editor.left_col = cursor_col - cols + 1;
    }
}

static void editor_render_error(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = editor_io(ctx);

    solar_os_shell_io_clear(io);
    solar_os_shell_io_write_bold(io, "edit");
    solar_os_shell_io_newline(io);
    solar_os_shell_io_writeln(io, editor.message);
    solar_os_shell_io_writeln(io, "ESC");
    solar_os_shell_io_flush(io);
}

static void editor_render(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = editor_io(ctx);
    const size_t rows = solar_os_shell_io_rows(io);
    const size_t cols = solar_os_shell_io_cols(io);
    const size_t text_rows = rows > 1 ? rows - 1 : 1;
    const size_t cursor_line = editor_line_for_index(editor.cursor);
    const size_t cursor_col = editor_cursor_col();
    solar_os_syntax_state_t syntax_state;
    editor_attrs_t attrs = {0};
    size_t selection_start = 0;
    size_t selection_end = 0;
    const bool has_selection = editor_has_selection();
    char header[192];

    if (editor.error_only) {
        editor_render_error(ctx);
        return;
    }

    if (has_selection) {
        editor_selection_bounds(&selection_start, &selection_end);
    }

    editor_ensure_cursor_visible(io);
    solar_os_shell_io_clear(io);

    if (editor.message[0] != '\0') {
        snprintf(header,
                 sizeof(header),
                 "edit %s%s  %u:%u  %s",
                 editor.display_name,
                 editor.dirty ? " *" : "",
                 (unsigned)(cursor_line + 1),
                 (unsigned)(cursor_col + 1),
                 editor.message);
    } else {
        snprintf(header,
                 sizeof(header),
                 "edit %s%s  %u:%u",
                 editor.display_name,
                 editor.dirty ? " *" : "",
                 (unsigned)(cursor_line + 1),
                 (unsigned)(cursor_col + 1));
    }

    solar_os_shell_io_set_cursor(io, 0, 0);
    editor_write_clipped(io, header, cols, true);
    editor_reset_style(io);
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

        solar_os_shell_io_set_cursor(io, row + 1, 0);
        for (size_t col = 0; col < copy_len; col++) {
            const size_t index = visible_start + col;
            const bool selected = has_selection && index >= selection_start && index < selection_end;
            const solar_os_syntax_style_t style = (solar_os_syntax_style_t)styles[col];
            editor_apply_style(io, &attrs, style, selected);
            solar_os_shell_io_put_char(io, editor_is_printable(line[col]) ? line[col] : '.');
        }
        editor_reset_style(io);
        memset(&attrs, 0, sizeof(attrs));
    }
    editor_reset_style(io);

    if (rows > 1 &&
        cursor_line >= editor.top_line &&
        cursor_line < editor.top_line + text_rows &&
        cursor_col >= editor.left_col) {
        const size_t screen_col = cursor_col - editor.left_col;
        solar_os_shell_io_set_cursor(io, cursor_line - editor.top_line + 1, screen_col);
    }
    solar_os_shell_io_flush(io);
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
    editor_update_preferred_col();
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
        editor_set_message("buffer full");
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
    editor_update_preferred_col();
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
    editor_update_preferred_col();
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
    editor_update_preferred_col();
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

static void editor_page_up(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = editor_io(ctx);
    const size_t rows = solar_os_shell_io_rows(io);
    const size_t page = rows > 1 ? rows - 1 : 1;

    for (size_t i = 0; i < page; i++) {
        editor_move_up();
    }
}

static void editor_page_down(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = editor_io(ctx);
    const size_t rows = solar_os_shell_io_rows(io);
    const size_t page = rows > 1 ? rows - 1 : 1;

    for (size_t i = 0; i < page; i++) {
        editor_move_down();
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
        editor_set_message("buffer full");
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
    editor_update_preferred_col();
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
    editor_update_preferred_col();
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
    editor.dirty = false;
    editor.error_only = false;
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
        editor.error_only = true;
        return ESP_OK;
    }

    editor.len = fread(editor.buffer, 1, editor.capacity - 1, file);
    if (ferror(file)) {
        char message[sizeof(editor.message)];
        snprintf(message, sizeof(message), "read failed: %s", strerror(errno));
        fclose(file);
        editor_set_message(message);
        editor.error_only = true;
        return ESP_OK;
    }

    const int extra = fgetc(file);
    fclose(file);
    if (extra != EOF) {
        editor.len = 0;
        editor.buffer[0] = '\0';
        editor_set_message("file too large");
        editor.error_only = true;
        return ESP_OK;
    }

    editor.buffer[editor.len] = '\0';
    editor.cursor = 0;
    editor.preferred_col = 0;
    editor.top_line = 0;
    editor.left_col = 0;
    editor.selection_anchor = 0;
    editor.selection_active = false;
    editor.dirty = false;
    editor.error_only = false;
    editor_set_message("");
    return ESP_OK;
}

static esp_err_t edit_start(solar_os_context_t *ctx)
{
    memset(&editor, 0, sizeof(editor));

    editor.buffer = solar_os_memory_alloc(EDITOR_BUFFER_CAPACITY,
                                           SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                           "edit.buffer");
    if (editor.buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    editor.capacity = EDITOR_BUFFER_CAPACITY;
    editor_capture_text_size(ctx);

    const int argc = solar_os_context_argc(ctx);
    if (argc != 2) {
        editor.error_only = true;
        editor_set_message("usage: edit <file>");
        editor_render(ctx);
        return ESP_OK;
    }

    if (!solar_os_storage_is_mounted()) {
        editor.error_only = true;
        editor_set_message("storage not mounted");
        editor_render(ctx);
        return ESP_OK;
    }

    const char *arg = solar_os_context_argv(ctx, 1);
    const esp_err_t path_err = solar_os_storage_resolve_path(arg,
                                                             editor.path,
                                                             sizeof(editor.path));
    if (path_err != ESP_OK) {
        editor.error_only = true;
        editor_set_message(path_err == ESP_ERR_INVALID_SIZE ? "path too long" : "invalid path");
        editor_render(ctx);
        return ESP_OK;
    }
    strlcpy(editor.display_name, arg != NULL ? arg : editor.path, sizeof(editor.display_name));
    editor.syntax = solar_os_syntax_language_for_path(editor.path);

    const esp_err_t err = editor_open_file();
    if (err != ESP_OK) {
        return err;
    }

    editor_render(ctx);
    return ESP_OK;
}

static void edit_stop(solar_os_context_t *ctx)
{
    editor_restore_text_size(ctx);

    heap_caps_free(editor.buffer);
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
                 "edit %s%s",
                 editor.display_name,
                 editor.dirty ? "*" : "");
        return;
    }
    strlcpy(buffer, "edit", buffer_len);
}

static void editor_apply_move(bool selecting, void (*move)(void))
{
    editor_begin_selection(selecting);
    move();
    editor_finish_selection(selecting);
}

static void editor_apply_page_move(solar_os_context_t *ctx, bool selecting, bool down)
{
    editor_begin_selection(selecting);
    if (down) {
        editor_page_down(ctx);
    } else {
        editor_page_up(ctx);
    }
    editor_finish_selection(selecting);
}

static bool edit_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL || event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const char ch = event->data.ch;
    if ((uint8_t)ch == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_request_exit(ctx);
        return true;
    }

    if (editor.error_only) {
        if (ch == SOLAR_OS_KEY_ESCAPE) {
            solar_os_context_request_exit(ctx);
        }
        return true;
    }

    switch ((uint8_t)ch) {
    case SOLAR_OS_KEY_ESCAPE:
        if (!editor.dirty || editor_save() == ESP_OK) {
            solar_os_context_request_exit(ctx);
        }
        break;
    case 0x01:
        editor_select_all();
        break;
    case 0x03:
        editor_copy_selection();
        break;
    case 0x16:
        editor_paste_clipboard();
        break;
    case 0x18:
        editor_cut_selection();
        break;
    case SOLAR_OS_KEY_CTRL_PLUS:
        editor_adjust_text_size(ctx, 1);
        break;
    case SOLAR_OS_KEY_CTRL_MINUS:
        editor_adjust_text_size(ctx, -1);
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
        editor_apply_page_move(ctx, false, false);
        break;
    case SOLAR_OS_KEY_SHIFT_PAGE_UP:
        editor_apply_page_move(ctx, true, false);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        editor_apply_page_move(ctx, false, true);
        break;
    case SOLAR_OS_KEY_SHIFT_PAGE_DOWN:
        editor_apply_page_move(ctx, true, true);
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
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = edit_start,
    .resume = edit_resume,
    .stop = edit_stop,
    .event = edit_event,
    .title = edit_title,
};
