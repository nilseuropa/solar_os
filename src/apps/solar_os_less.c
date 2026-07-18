#include "solar_os_less.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "solar_os_keys.h"
#include "solar_os_memory.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"

#define LESS_MAX_BYTES (2U * 1024U * 1024U)
#define LESS_SEARCH_MAX 64
#define LESS_MESSAGE_MAX 72
#define LESS_TAB_WIDTH 4

typedef enum {
    LESS_INPUT_NORMAL,
    LESS_INPUT_SEARCH,
} less_input_mode_t;

typedef struct {
    char *buffer;
    size_t len;
    size_t top_offset;
    size_t match_offset;
    bool match_valid;
    bool error_only;
    less_input_mode_t input_mode;
    const char *app_name;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char display_name[SOLAR_OS_STORAGE_PATH_MAX];
    char search[LESS_SEARCH_MAX];
    size_t search_len;
    char message[LESS_MESSAGE_MAX];
} less_state_t;

static less_state_t less_state;
static solar_os_shell_io_t less_fallback_io;

static solar_os_shell_io_t *less_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&less_fallback_io, solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &less_fallback_io);
        io = &less_fallback_io;
    }
    return io;
}

static const char *less_app_name(void)
{
    return less_state.app_name != NULL ? less_state.app_name : "less";
}

static bool less_is_printable(uint8_t ch)
{
    return isprint(ch) || ch >= 0xa0;
}

static void less_set_message(const char *message)
{
    strlcpy(less_state.message, message != NULL ? message : "", sizeof(less_state.message));
}

static size_t less_text_rows(solar_os_shell_io_t *io)
{
    const size_t rows = solar_os_shell_io_rows(io);
    return rows > 1 ? rows - 1 : 1;
}

static size_t less_cols_or_one(solar_os_shell_io_t *io)
{
    const size_t cols = solar_os_shell_io_cols(io);
    return cols > 0 ? cols : 1;
}

static size_t less_line_end(size_t offset)
{
    while (offset < less_state.len &&
           less_state.buffer[offset] != '\n' &&
           less_state.buffer[offset] != '\r') {
        offset++;
    }
    return offset;
}

static size_t less_next_line_start(size_t offset)
{
    size_t end = less_line_end(offset);
    if (end >= less_state.len) {
        return less_state.len;
    }
    if (less_state.buffer[end] == '\r' &&
        end + 1 < less_state.len &&
        less_state.buffer[end + 1] == '\n') {
        return end + 2;
    }
    return end + 1;
}

static size_t less_previous_line_start(size_t offset)
{
    if (offset > less_state.len) {
        offset = less_state.len;
    }
    if (offset == 0) {
        return 0;
    }

    if (less_state.buffer[offset - 1] == '\n') {
        offset--;
        if (offset > 0 && less_state.buffer[offset - 1] == '\r') {
            offset--;
        }
    } else if (less_state.buffer[offset - 1] == '\r') {
        offset--;
    } else {
        while (offset > 0 &&
               less_state.buffer[offset - 1] != '\n' &&
               less_state.buffer[offset - 1] != '\r') {
            offset--;
        }
        if (offset == 0) {
            return 0;
        }
        if (less_state.buffer[offset - 1] == '\n') {
            offset--;
            if (offset > 0 && less_state.buffer[offset - 1] == '\r') {
                offset--;
            }
        } else if (less_state.buffer[offset - 1] == '\r') {
            offset--;
        }
    }

    while (offset > 0 &&
           less_state.buffer[offset - 1] != '\n' &&
           less_state.buffer[offset - 1] != '\r') {
        offset--;
    }
    return offset;
}

static size_t less_line_start_for(size_t offset)
{
    if (offset > less_state.len) {
        offset = less_state.len;
    }
    while (offset > 0 &&
           less_state.buffer[offset - 1] != '\n' &&
           less_state.buffer[offset - 1] != '\r') {
        offset--;
    }
    return offset;
}

static bool less_is_wrap_space(uint8_t ch)
{
    return ch == ' ' || ch == '\t';
}

static size_t less_cell_width(uint8_t ch, size_t col)
{
    if (ch == '\t') {
        return LESS_TAB_WIDTH - (col % LESS_TAB_WIDTH);
    }
    return 1;
}

static size_t less_physical_visual_end(size_t offset, size_t cols, size_t *next_offset)
{
    if (cols == 0) {
        cols = 1;
    }
    if (next_offset != NULL) {
        *next_offset = offset;
    }
    if (offset >= less_state.len) {
        if (next_offset != NULL) {
            *next_offset = less_state.len;
        }
        return less_state.len;
    }

    const size_t line_end = less_line_end(offset);
    if (offset >= line_end) {
        const size_t next = less_next_line_start(offset);
        if (next_offset != NULL) {
            *next_offset = next > offset ? next : less_state.len;
        }
        return offset;
    }

    size_t col = 0;
    size_t end = offset;
    size_t last_space = SIZE_MAX;
    bool saw_nonspace = false;

    for (size_t i = offset; i < line_end; i++) {
        const uint8_t byte = (uint8_t)less_state.buffer[i];
        const size_t width = less_cell_width(byte, col);
        if (col > 0 && col + width > cols) {
            break;
        }

        col += width;
        end = i + 1;

        if (less_is_wrap_space(byte)) {
            if (saw_nonspace) {
                last_space = i;
            }
        } else {
            saw_nonspace = true;
        }

        if (col >= cols) {
            break;
        }
    }

    if (end >= line_end) {
        if (next_offset != NULL) {
            *next_offset = less_next_line_start(offset);
        }
        return line_end;
    }

    if (last_space != SIZE_MAX && last_space > offset) {
        size_t next = last_space + 1;
        while (next < line_end && less_is_wrap_space((uint8_t)less_state.buffer[next])) {
            next++;
        }
        if (next > offset) {
            if (next_offset != NULL) {
                *next_offset = next;
            }
            return last_space;
        }
    }

    if (end <= offset) {
        end = offset + 1;
    }
    if (next_offset != NULL) {
        size_t next = end;
        if (end > offset) {
            while (next < line_end && less_is_wrap_space((uint8_t)less_state.buffer[next])) {
                next++;
            }
        }
        *next_offset = next > offset ? next : end;
    }
    return end;
}

static size_t less_visual_end(size_t offset, size_t cols, size_t *next_offset)
{
    return less_physical_visual_end(offset, cols, next_offset);
}

static size_t less_next_visual_start(size_t offset, size_t cols)
{
    size_t next = offset;
    (void)less_visual_end(offset, cols, &next);
    if (next <= offset && offset < less_state.len) {
        next = offset + 1;
    }
    return next;
}

static size_t less_visual_start_for_offset(size_t offset, size_t cols)
{
    if (offset > less_state.len) {
        offset = less_state.len;
    }

    size_t current = less_line_start_for(offset);
    while (current < offset) {
        const size_t next = less_next_visual_start(current, cols);
        if (next > offset || next <= current) {
            break;
        }
        current = next;
    }
    return current;
}

static size_t less_previous_visual_start(size_t offset, size_t cols)
{
    if (offset == 0 || less_state.len == 0) {
        return 0;
    }
    if (offset > less_state.len) {
        offset = less_state.len;
    }
    size_t line_start = less_line_start_for(offset);
    if (line_start == offset) {
        const size_t previous_line = less_previous_line_start(offset);
        if (previous_line == offset) {
            return offset;
        }
        line_start = previous_line;
    }

    size_t previous = line_start;
    size_t current = line_start;
    while (current < offset) {
        const size_t next = less_next_visual_start(current, cols);
        if (next >= offset || next <= current) {
            return previous;
        }
        previous = current;
        current = next;
    }
    return previous;
}

static void less_write_inverse_line(solar_os_shell_io_t *io, const char *text)
{
    const size_t cols = solar_os_shell_io_cols(io);
    size_t written = 0;

    solar_os_shell_io_set_cursor(io, 0, 0);
    solar_os_shell_io_set_inverse(io, true);
    while (text != NULL && text[written] != '\0' && written < cols) {
        solar_os_shell_io_put_char(io,
                                   less_is_printable((uint8_t)text[written]) ?
                                       text[written] :
                                       '.');
        written++;
    }
    while (written < cols) {
        solar_os_shell_io_put_char(io, ' ');
        written++;
    }
    solar_os_shell_io_set_inverse(io, false);
}

static void less_render_header(solar_os_shell_io_t *io)
{
    char header[SOLAR_OS_STORAGE_PATH_MAX + LESS_SEARCH_MAX + LESS_MESSAGE_MAX + 32];
    const unsigned percent =
        less_state.len == 0 ? 100U : (unsigned)(((uint64_t)less_state.top_offset * 100ULL) /
                                                (uint64_t)less_state.len);

    if (less_state.input_mode == LESS_INPUT_SEARCH) {
        snprintf(header,
                 sizeof(header),
                 "%s %s  /%s",
                 less_app_name(),
                 less_state.display_name,
                 less_state.search);
    } else if (less_state.message[0] != '\0') {
        snprintf(header,
                 sizeof(header),
                 "%s %s  %u%%  %s",
                 less_app_name(),
                 less_state.display_name,
                 percent,
                 less_state.message);
    } else {
        snprintf(header,
                 sizeof(header),
                 "%s %s  %u%%",
                 less_app_name(),
                 less_state.display_name,
                 percent);
    }

    less_write_inverse_line(io, header);
}

static bool less_source_highlighted(size_t offset)
{
    return less_state.match_valid &&
        offset >= less_state.match_offset &&
        offset < less_state.match_offset + less_state.search_len;
}

static void less_write_source_byte(solar_os_shell_io_t *io,
                                   uint8_t byte,
                                   size_t source_offset,
                                   size_t *visual_col,
                                   size_t *written,
                                   size_t cols)
{
    const bool is_tab = byte == '\t';
    const size_t span = less_cell_width(byte, *visual_col);

    solar_os_shell_io_set_inverse(io, less_source_highlighted(source_offset));
    for (size_t j = 0; j < span && *written < cols; j++) {
        if (is_tab) {
            solar_os_shell_io_put_char(io, ' ');
        } else {
            solar_os_shell_io_put_char(io, less_is_printable(byte) ? (char)byte : '.');
        }
        (*written)++;
        (*visual_col)++;
    }
}

static void less_render_physical_text_row(solar_os_shell_io_t *io, size_t row, size_t row_start)
{
    const size_t cols = less_cols_or_one(io);
    size_t next = row_start;
    const size_t row_end = less_physical_visual_end(row_start, cols, &next);
    size_t visual_col = 0;
    size_t written = 0;

    solar_os_shell_io_set_cursor(io, row, 0);
    for (size_t i = row_start; i < row_end && written < cols; i++) {
        const uint8_t byte = (uint8_t)less_state.buffer[i];
        less_write_source_byte(io, byte, i, &visual_col, &written, cols);
    }
    solar_os_shell_io_set_inverse(io, false);
}

static void less_render_text_row(solar_os_shell_io_t *io, size_t row, size_t row_start)
{
    less_render_physical_text_row(io, row, row_start);
}

static void less_render_error(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = less_io(ctx);

    solar_os_shell_io_clear(io);
    less_write_inverse_line(io, less_app_name());
    solar_os_shell_io_set_cursor(io, 1, 0);
    if (less_state.message[0] != '\0') {
        solar_os_shell_io_writeln(io, less_state.message);
    }
    solar_os_shell_io_printf(io, "usage: %s <file>\n", less_app_name());
    solar_os_shell_io_writeln(io, "keys: arrows, PgUp/PgDn, / search, n/N, q");
    solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
    solar_os_shell_io_flush(io);
}

static void less_render(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = less_io(ctx);

    if (less_state.error_only) {
        less_render_error(ctx);
        return;
    }

    solar_os_shell_io_clear(io);
    less_render_header(io);

    size_t offset = less_state.top_offset;
    const size_t rows = less_text_rows(io);
    const size_t cols = less_cols_or_one(io);
    for (size_t row = 0; row < rows; row++) {
        if (offset < less_state.len || (less_state.len == 0 && row == 0)) {
            less_render_text_row(io, row + 1, offset);
        }
        if (offset >= less_state.len) {
            continue;
        }
        offset = less_next_visual_start(offset, cols);
    }
    solar_os_shell_io_flush(io);
}

static void less_move_down(solar_os_context_t *ctx, size_t lines)
{
    const size_t cols = less_cols_or_one(less_io(ctx));

    for (size_t i = 0; i < lines; i++) {
        const size_t next = less_next_visual_start(less_state.top_offset, cols);
        if (next >= less_state.len) {
            break;
        }
        less_state.top_offset = next;
    }
    less_set_message("");
}

static void less_move_up(solar_os_context_t *ctx, size_t lines)
{
    const size_t cols = less_cols_or_one(less_io(ctx));

    for (size_t i = 0; i < lines; i++) {
        const size_t previous = less_previous_visual_start(less_state.top_offset, cols);
        if (previous == less_state.top_offset) {
            break;
        }
        less_state.top_offset = previous;
        if (less_state.top_offset == 0) {
            break;
        }
    }
    less_set_message("");
}

static void less_page_down(solar_os_context_t *ctx)
{
    const size_t rows = less_text_rows(less_io(ctx));
    less_move_down(ctx, rows > 1 ? rows - 1 : 1);
}

static void less_page_up(solar_os_context_t *ctx)
{
    const size_t rows = less_text_rows(less_io(ctx));
    less_move_up(ctx, rows > 1 ? rows - 1 : 1);
}

static void less_move_bottom(solar_os_context_t *ctx)
{
    size_t offset = less_state.len;
    solar_os_shell_io_t *io = less_io(ctx);
    const size_t rows = less_text_rows(io);
    const size_t cols = less_cols_or_one(io);

    for (size_t i = 0; i < rows; i++) {
        const size_t previous = less_previous_visual_start(offset, cols);
        offset = previous;
        if (offset == 0) {
            break;
        }
    }
    less_state.top_offset = offset;
    less_set_message("");
}

static bool less_search_char_equal(char a, char b)
{
    return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static bool less_search_matches_at(size_t offset)
{
    if (less_state.search_len == 0 || offset + less_state.search_len > less_state.len) {
        return false;
    }

    for (size_t i = 0; i < less_state.search_len; i++) {
        if (!less_search_char_equal(less_state.buffer[offset + i], less_state.search[i])) {
            return false;
        }
    }
    return true;
}

static bool less_find_forward(size_t start, size_t *found)
{
    if (less_state.search_len == 0 || less_state.search_len > less_state.len) {
        return false;
    }
    if (start > less_state.len - less_state.search_len) {
        return false;
    }

    for (size_t i = start; i <= less_state.len - less_state.search_len; i++) {
        if (less_search_matches_at(i)) {
            *found = i;
            return true;
        }
    }
    return false;
}

static bool less_find_backward(size_t start, size_t *found)
{
    if (less_state.search_len == 0 || less_state.search_len > less_state.len) {
        return false;
    }
    if (start > less_state.len - less_state.search_len) {
        start = less_state.len - less_state.search_len;
    }

    for (size_t i = start + 1; i > 0; i--) {
        const size_t offset = i - 1;
        if (less_search_matches_at(offset)) {
            *found = offset;
            return true;
        }
    }
    return false;
}

static void less_apply_match(solar_os_context_t *ctx, size_t offset)
{
    const size_t cols = less_cols_or_one(less_io(ctx));

    less_state.match_offset = offset;
    less_state.match_valid = true;
    less_state.top_offset = less_visual_start_for_offset(offset, cols);
}

static bool less_find_search(solar_os_context_t *ctx, bool forward, bool next)
{
    size_t found = 0;
    bool wrapped = false;
    bool ok = false;

    if (less_state.search_len == 0) {
        less_set_message("empty search");
        return false;
    }

    if (forward) {
        size_t start = less_state.top_offset;
        if (next && less_state.match_valid && less_state.match_offset + 1 < less_state.len) {
            start = less_state.match_offset + 1;
        }
        ok = less_find_forward(start, &found);
        if (!ok && start > 0) {
            wrapped = true;
            ok = less_find_forward(0, &found);
        }
    } else {
        size_t start = less_state.top_offset;
        if (next && less_state.match_valid && less_state.match_offset > 0) {
            start = less_state.match_offset - 1;
        } else if (start > 0) {
            start--;
        }
        ok = less_find_backward(start, &found);
        if (!ok && less_state.len >= less_state.search_len) {
            wrapped = true;
            ok = less_find_backward(less_state.len - less_state.search_len, &found);
        }
    }

    if (!ok) {
        less_state.match_valid = false;
        less_set_message("not found");
        return false;
    }

    less_apply_match(ctx, found);
    less_set_message(wrapped ? "wrapped" : "");
    return true;
}

static void less_start_search(void)
{
    less_state.input_mode = LESS_INPUT_SEARCH;
    less_state.search_len = 0;
    less_state.search[0] = '\0';
    less_set_message("");
}

static void less_submit_search(solar_os_context_t *ctx)
{
    less_state.input_mode = LESS_INPUT_NORMAL;
    less_state.match_valid = false;
    (void)less_find_search(ctx, true, false);
}

static void less_cancel_search(void)
{
    less_state.input_mode = LESS_INPUT_NORMAL;
    less_set_message("");
}

static bool less_handle_search_input(solar_os_context_t *ctx, uint8_t ch)
{
    switch (ch) {
    case SOLAR_OS_KEY_ESCAPE:
        less_cancel_search();
        break;
    case '\r':
    case '\n':
        less_submit_search(ctx);
        break;
    case '\b':
        if (less_state.search_len > 0) {
            less_state.search[--less_state.search_len] = '\0';
        }
        break;
    default:
        if (less_is_printable(ch) && less_state.search_len + 1 < sizeof(less_state.search)) {
            less_state.search[less_state.search_len++] = (char)ch;
            less_state.search[less_state.search_len] = '\0';
        }
        break;
    }

    less_render(ctx);
    return true;
}

static esp_err_t less_load_file(void)
{
    struct stat st;
    if (stat(less_state.path, &st) != 0 || !S_ISREG(st.st_mode)) {
        less_set_message("not a file");
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > LESS_MAX_BYTES) {
        less_set_message("file too large");
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t len = (size_t)st.st_size;
    less_state.buffer = solar_os_memory_alloc(len + 1,
                                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                               "less.file");
    if (less_state.buffer == NULL) {
        less_set_message("out of memory");
        return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(less_state.path, "rb");
    if (file == NULL) {
        char message[sizeof(less_state.message)];
        snprintf(message, sizeof(message), "open failed: %s", strerror(errno));
        less_set_message(message);
        return ESP_FAIL;
    }

    const size_t read_len = len > 0 ? fread(less_state.buffer, 1, len, file) : 0;
    const bool read_error = ferror(file);
    fclose(file);
    if (read_error || read_len != len) {
        less_set_message("read failed");
        return ESP_FAIL;
    }

    less_state.buffer[len] = '\0';
    less_state.len = len;
    return ESP_OK;
}

static esp_err_t less_start_common(solar_os_context_t *ctx, const char *app_name)
{
    memset(&less_state, 0, sizeof(less_state));
    less_state.app_name = app_name;

    const int argc = solar_os_context_argc(ctx);
    if (argc != 2) {
        less_state.error_only = true;
        char message[LESS_MESSAGE_MAX];
        snprintf(message, sizeof(message), "usage: %s <file>", less_app_name());
        less_set_message(message);
        less_render(ctx);
        return ESP_OK;
    }

    const char *arg = solar_os_context_argv(ctx, 1);
    strlcpy(less_state.display_name, arg != NULL ? arg : "", sizeof(less_state.display_name));

    esp_err_t err = solar_os_storage_resolve_path(arg, less_state.path, sizeof(less_state.path));
    if (err != ESP_OK) {
        less_state.error_only = true;
        less_set_message(err == ESP_ERR_INVALID_SIZE ? "path too long" : "invalid path");
        less_render(ctx);
        return ESP_OK;
    }

    err = less_load_file();
    if (err != ESP_OK) {
        less_state.error_only = true;
        less_render(ctx);
        return ESP_OK;
    }

    less_state.top_offset = 0;
    less_render(ctx);
    return ESP_OK;
}

static esp_err_t less_start(solar_os_context_t *ctx)
{
    return less_start_common(ctx, "less");
}

static void less_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_memory_free(less_state.buffer);
    memset(&less_state, 0, sizeof(less_state));
}

static bool less_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL || event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_request_exit(ctx);
        return true;
    }

    if (less_state.error_only) {
        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
        }
        return true;
    }

    if (less_state.input_mode == LESS_INPUT_SEARCH) {
        return less_handle_search_input(ctx, ch);
    }

    switch (ch) {
    case SOLAR_OS_KEY_ESCAPE:
    case 'q':
    case 'Q':
        solar_os_context_request_exit(ctx);
        return true;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        less_move_down(ctx, 1);
        break;
    case SOLAR_OS_KEY_UP:
    case 'k':
        less_move_up(ctx, 1);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
    case ' ':
    case 0x06:
        less_page_down(ctx);
        break;
    case SOLAR_OS_KEY_PAGE_UP:
    case 'b':
    case 'B':
    case 0x02:
        less_page_up(ctx);
        break;
    case SOLAR_OS_KEY_HOME:
    case 'g':
        less_state.top_offset = 0;
        less_set_message("");
        break;
    case SOLAR_OS_KEY_END:
    case 'G':
        less_move_bottom(ctx);
        break;
    case '/':
        less_start_search();
        break;
    case 'n':
        (void)less_find_search(ctx, true, true);
        break;
    case 'N':
        (void)less_find_search(ctx, false, true);
        break;
    default:
        return true;
    }

    less_render(ctx);
    return true;
}

const solar_os_app_t solar_os_less_app = {
    .name = "less",
    .summary = "text file pager",
    .start = less_start,
    .stop = less_stop,
    .event = less_event,
};
