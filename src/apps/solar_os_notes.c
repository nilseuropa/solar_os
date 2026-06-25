#include "solar_os_notes.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "solar_os_keys.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"

#define NOTES_DEFAULT_PATH "/.notes/default.md"
#define NOTES_DEFAULT_DIR "/.notes"
#define NOTES_MAX_ITEMS 256U
#define NOTES_MAX_PREAMBLE_LINES 64U
#define NOTES_TEXT_MAX 128U
#define NOTES_LINE_MAX 192U
#define NOTES_MESSAGE_MAX 96U
#define NOTES_PREAMBLE_VISIBLE_MAX 3U

typedef enum {
    NOTES_INPUT_NONE,
    NOTES_INPUT_ADD,
    NOTES_INPUT_EDIT,
} notes_input_mode_t;

typedef struct {
    uint32_t id;
    bool checked;
    char text[NOTES_TEXT_MAX];
} notes_item_t;

typedef struct {
    bool running;
    bool error_only;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char display_name[SOLAR_OS_STORAGE_PATH_MAX];
    char message[NOTES_MESSAGE_MAX];
    notes_item_t *items;
    notes_item_t *scratch;
    char (*preamble)[NOTES_TEXT_MAX];
    size_t item_count;
    size_t preamble_count;
    size_t done_start;
    size_t cursor;
    size_t top;
    uint32_t next_id;
    notes_input_mode_t input_mode;
    char input[NOTES_TEXT_MAX];
    size_t input_len;
} notes_state_t;

static notes_state_t notes;
static solar_os_shell_io_t notes_fallback_io;

static solar_os_shell_io_t *notes_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&notes_fallback_io, solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &notes_fallback_io);
        io = &notes_fallback_io;
    }
    return io;
}

static void notes_set_message(const char *message)
{
    strlcpy(notes.message, message != NULL ? message : "", sizeof(notes.message));
}

static void *notes_calloc(size_t count, size_t size)
{
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void notes_free_buffers(void)
{
    if (notes.items != NULL) {
        heap_caps_free(notes.items);
    }
    if (notes.scratch != NULL) {
        heap_caps_free(notes.scratch);
    }
    if (notes.preamble != NULL) {
        heap_caps_free(notes.preamble);
    }
    notes.items = NULL;
    notes.scratch = NULL;
    notes.preamble = NULL;
}

static bool notes_alloc_buffers(void)
{
    notes.items = notes_calloc(NOTES_MAX_ITEMS, sizeof(notes.items[0]));
    notes.scratch = notes_calloc(NOTES_MAX_ITEMS, sizeof(notes.scratch[0]));
    notes.preamble = notes_calloc(NOTES_MAX_PREAMBLE_LINES, sizeof(notes.preamble[0]));
    if (notes.items == NULL || notes.scratch == NULL || notes.preamble == NULL) {
        notes_free_buffers();
        return false;
    }
    return true;
}

static void notes_trim_line(char *line)
{
    if (line == NULL) {
        return;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1U] == '\n' || line[len - 1U] == '\r')) {
        line[--len] = '\0';
    }
}

static const char *notes_skip_space(const char *text)
{
    while (text != NULL && (*text == ' ' || *text == '\t')) {
        text++;
    }
    return text;
}

static bool notes_parse_check_item(const char *line, bool *checked, const char **text)
{
    const char *p = notes_skip_space(line);
    if (p == NULL || (*p != '-' && *p != '*')) {
        return false;
    }
    p++;
    p = notes_skip_space(p);
    if (p[0] != '[' || p[2] != ']' || (p[1] != ' ' && p[1] != 'x' && p[1] != 'X')) {
        return false;
    }
    if (checked != NULL) {
        *checked = p[1] == 'x' || p[1] == 'X';
    }
    p += 3;
    p = notes_skip_space(p);
    if (text != NULL) {
        *text = p;
    }
    return true;
}

static bool notes_add_item(bool checked, const char *text)
{
    if (notes.item_count >= NOTES_MAX_ITEMS) {
        notes_set_message("item limit reached");
        return false;
    }

    notes_item_t *item = &notes.items[notes.item_count++];
    item->id = notes.next_id++;
    item->checked = checked;
    strlcpy(item->text, text != NULL ? text : "", sizeof(item->text));
    return true;
}

static void notes_reorder(uint32_t keep_id)
{
    size_t out = 0;
    for (size_t i = 0; i < notes.item_count; i++) {
        if (!notes.items[i].checked) {
            notes.scratch[out++] = notes.items[i];
        }
    }
    notes.done_start = out;
    for (size_t i = 0; i < notes.item_count; i++) {
        if (notes.items[i].checked) {
            notes.scratch[out++] = notes.items[i];
        }
    }
    memcpy(notes.items, notes.scratch, notes.item_count * sizeof(notes.items[0]));

    if (notes.item_count == 0) {
        notes.cursor = 0;
        notes.top = 0;
        return;
    }
    if (keep_id != 0) {
        for (size_t i = 0; i < notes.item_count; i++) {
            if (notes.items[i].id == keep_id) {
                notes.cursor = i;
                return;
            }
        }
    }
    if (notes.cursor >= notes.item_count) {
        notes.cursor = notes.item_count - 1U;
    }
}

static bool notes_has_separator(void)
{
    return notes.done_start > 0 && notes.done_start < notes.item_count;
}

static size_t notes_virtual_count(void)
{
    return notes.item_count + (notes_has_separator() ? 1U : 0U);
}

static size_t notes_cursor_virtual(void)
{
    if (!notes_has_separator()) {
        return notes.cursor;
    }
    return notes.cursor >= notes.done_start ? notes.cursor + 1U : notes.cursor;
}

static bool notes_virtual_is_separator(size_t index)
{
    return notes_has_separator() && index == notes.done_start;
}

static size_t notes_item_from_virtual(size_t index)
{
    return notes_has_separator() && index > notes.done_start ? index - 1U : index;
}

static size_t notes_preamble_visible_rows(size_t body_rows)
{
    size_t visible = notes.preamble_count;
    if (visible > NOTES_PREAMBLE_VISIBLE_MAX) {
        visible = NOTES_PREAMBLE_VISIBLE_MAX;
    }
    if (visible > body_rows) {
        visible = body_rows;
    }
    return visible;
}

static size_t notes_list_rows(solar_os_shell_io_t *io)
{
    const size_t rows = solar_os_shell_io_rows(io);
    if (rows <= 3) {
        return 0;
    }
    const size_t body = rows - 2U;
    const size_t preamble_rows = notes_preamble_visible_rows(body);
    return body > preamble_rows ? body - preamble_rows : 0;
}

static void notes_ensure_visible(solar_os_shell_io_t *io)
{
    const size_t list_rows = notes_list_rows(io);
    if (list_rows == 0 || notes.item_count == 0) {
        notes.top = 0;
        return;
    }

    const size_t cursor_v = notes_cursor_virtual();
    if (cursor_v < notes.top) {
        notes.top = cursor_v;
    } else if (cursor_v >= notes.top + list_rows) {
        notes.top = cursor_v - list_rows + 1U;
    }

    const size_t total = notes_virtual_count();
    if (notes.top >= total) {
        notes.top = total > 0 ? total - 1U : 0;
    }
}

static void notes_write_clipped(solar_os_shell_io_t *io, const char *text, size_t width)
{
    if (width == 0) {
        return;
    }
    const size_t len = text != NULL ? strlen(text) : 0;
    const size_t write_len = len < width ? len : width;
    if (write_len > 0) {
        solar_os_shell_io_write_len(io, text, write_len);
    }
    for (size_t i = write_len; i < width; i++) {
        solar_os_shell_io_put_char(io, ' ');
    }
}

static void notes_render(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = notes_io(ctx);
    const size_t rows = solar_os_shell_io_rows(io);
    const size_t cols = solar_os_shell_io_cols(io);
    if (rows == 0 || cols == 0) {
        return;
    }

    notes_ensure_visible(io);
    solar_os_shell_io_clear(io);
    solar_os_shell_io_set_cursor_visible(io, false);

    solar_os_shell_io_set_cursor(io, 0, 0);
    solar_os_shell_io_set_inverse(io, true);
    char title[NOTES_LINE_MAX];
    snprintf(title,
             sizeof(title),
             "notes %s%s",
             notes.display_name[0] ? notes.display_name : notes.path,
             notes.error_only ? "" : "");
    notes_write_clipped(io, title, cols);
    solar_os_shell_io_set_inverse(io, false);

    size_t row = 1;
    const size_t body_rows = rows > 2 ? rows - 2U : 0U;
    const size_t preamble_rows = notes_preamble_visible_rows(body_rows);
    for (size_t i = 0; i < preamble_rows && row + 1U < rows; i++, row++) {
        solar_os_shell_io_set_cursor(io, row, 0);
        notes_write_clipped(io, notes.preamble[i], cols);
    }

    if (notes.item_count == 0 && !notes.error_only && row + 1U < rows) {
        solar_os_shell_io_set_cursor(io, row, 0);
        notes_write_clipped(io, "No checklist items", cols);
    }

    const size_t list_rows = rows > row + 1U ? rows - row - 1U : 0U;
    const size_t total = notes_virtual_count();
    for (size_t visible = 0; visible < list_rows && notes.top + visible < total; visible++) {
        const size_t virtual_index = notes.top + visible;
        solar_os_shell_io_set_cursor(io, row + visible, 0);
        if (notes_virtual_is_separator(virtual_index)) {
            notes_write_clipped(io, "---- done ----", cols);
            continue;
        }

        const size_t item_index = notes_item_from_virtual(virtual_index);
        if (item_index >= notes.item_count) {
            continue;
        }
        const notes_item_t *item = &notes.items[item_index];
        const bool selected = !notes.error_only && item_index == notes.cursor;
        solar_os_shell_io_set_inverse(io, selected);
        char line[NOTES_LINE_MAX];
        snprintf(line,
                 sizeof(line),
                 "%c [%c] %s",
                 selected ? '>' : ' ',
                 item->checked ? 'x' : ' ',
                 item->text);
        notes_write_clipped(io, line, cols);
        solar_os_shell_io_set_inverse(io, false);
    }

    solar_os_shell_io_set_cursor(io, rows - 1U, 0);
    if (notes.input_mode != NOTES_INPUT_NONE) {
        const char *label = notes.input_mode == NOTES_INPUT_ADD ? "add: " : "edit: ";
        solar_os_shell_io_write(io, label);
        const size_t label_len = strlen(label);
        const size_t input_width = cols > label_len ? cols - label_len : 0U;
        notes_write_clipped(io, notes.input, input_width);
        solar_os_shell_io_set_cursor(io,
                                     rows - 1U,
                                     label_len + (notes.input_len < input_width ?
                                         notes.input_len :
                                         input_width));
        solar_os_shell_io_set_cursor_visible(io, true);
    } else {
        notes_write_clipped(io, notes.message, cols);
    }
    solar_os_shell_io_flush(io);
}

static bool notes_ensure_default_dir(void)
{
    char dir[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_resolve_path(NOTES_DEFAULT_DIR, dir, sizeof(dir)) != ESP_OK) {
        return false;
    }
    if (solar_os_storage_mkdir(dir) == ESP_OK || errno == EEXIST) {
        return true;
    }
    return false;
}

static esp_err_t notes_save(void)
{
    FILE *file = fopen(notes.path, "w");
    if (file == NULL) {
        notes_set_message("save failed");
        return ESP_FAIL;
    }

    bool ok = true;
    for (size_t i = 0; i < notes.preamble_count; i++) {
        if (fprintf(file, "%s\n", notes.preamble[i]) < 0) {
            ok = false;
            break;
        }
    }
    if (ok && notes.preamble_count > 0 && notes.item_count > 0 &&
        notes.preamble[notes.preamble_count - 1U][0] != '\0') {
        ok = fprintf(file, "\n") >= 0;
    }
    for (size_t i = 0; ok && i < notes.item_count; i++) {
        ok = fprintf(file,
                     "- [%c] %s\n",
                     notes.items[i].checked ? 'x' : ' ',
                     notes.items[i].text) >= 0;
    }

    if (fclose(file) != 0) {
        ok = false;
    }
    notes_set_message(ok ? "saved" : "save failed");
    return ok ? ESP_OK : ESP_FAIL;
}

static void notes_add_preamble(const char *line)
{
    if (notes.preamble_count >= NOTES_MAX_PREAMBLE_LINES) {
        return;
    }
    strlcpy(notes.preamble[notes.preamble_count++],
            line != NULL ? line : "",
            NOTES_TEXT_MAX);
}

static esp_err_t notes_load(void)
{
    FILE *file = fopen(notes.path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            notes_set_message("new note");
            return ESP_OK;
        }
        notes_set_message("open failed");
        return ESP_FAIL;
    }

    char line[NOTES_LINE_MAX];
    while (fgets(line, sizeof(line), file) != NULL) {
        notes_trim_line(line);
        bool checked = false;
        const char *text = NULL;
        if (notes_parse_check_item(line, &checked, &text)) {
            (void)notes_add_item(checked, text);
        } else {
            notes_add_preamble(line);
        }
    }
    const bool read_ok = !ferror(file);
    fclose(file);
    notes_reorder(0);
    if (!read_ok) {
        notes_set_message("read failed");
        return ESP_FAIL;
    }
    if (notes.message[0] == '\0') {
        notes_set_message("");
    }
    return ESP_OK;
}

static void notes_start_add(void)
{
    notes.input_mode = NOTES_INPUT_ADD;
    notes.input[0] = '\0';
    notes.input_len = 0;
}

static void notes_start_edit(void)
{
    if (notes.item_count == 0 || notes.cursor >= notes.item_count) {
        return;
    }
    notes.input_mode = NOTES_INPUT_EDIT;
    strlcpy(notes.input, notes.items[notes.cursor].text, sizeof(notes.input));
    notes.input_len = strlen(notes.input);
}

static bool notes_is_printable(uint8_t ch)
{
    return isprint(ch) || ch >= 0xa0;
}

static void notes_finish_input(void)
{
    if (notes.input_mode == NOTES_INPUT_ADD) {
        if (notes.input_len > 0) {
            if (notes_add_item(false, notes.input)) {
                notes_reorder(notes.next_id - 1U);
                (void)notes_save();
            }
        }
    } else if (notes.input_mode == NOTES_INPUT_EDIT &&
               notes.item_count > 0 &&
               notes.cursor < notes.item_count) {
        strlcpy(notes.items[notes.cursor].text, notes.input, sizeof(notes.items[notes.cursor].text));
        (void)notes_save();
    }
    notes.input_mode = NOTES_INPUT_NONE;
    notes.input[0] = '\0';
    notes.input_len = 0;
}

static bool notes_handle_input(uint8_t ch)
{
    if (ch == SOLAR_OS_KEY_ESCAPE) {
        notes.input_mode = NOTES_INPUT_NONE;
        notes.input[0] = '\0';
        notes.input_len = 0;
        notes_set_message("cancelled");
        return true;
    }
    if (ch == '\r' || ch == '\n') {
        notes_finish_input();
        return true;
    }
    if (ch == 0x08 || ch == 0x7f) {
        if (notes.input_len > 0) {
            notes.input[--notes.input_len] = '\0';
        }
        return true;
    }
    if (notes_is_printable(ch) && notes.input_len + 1U < sizeof(notes.input)) {
        notes.input[notes.input_len++] = (char)ch;
        notes.input[notes.input_len] = '\0';
    }
    return true;
}

static void notes_move_down(void)
{
    if (notes.cursor + 1U < notes.item_count) {
        notes.cursor++;
    }
}

static void notes_move_up(void)
{
    if (notes.cursor > 0) {
        notes.cursor--;
    }
}

static bool notes_group_bounds(size_t index, size_t *start, size_t *end)
{
    if (index >= notes.item_count || start == NULL || end == NULL) {
        return false;
    }

    if (notes.items[index].checked) {
        *start = notes.done_start;
        *end = notes.item_count;
    } else {
        *start = 0;
        *end = notes.done_start;
    }
    return *start < *end;
}

static void notes_move_selected_item(bool down)
{
    if (notes.item_count == 0 || notes.cursor >= notes.item_count) {
        return;
    }

    size_t start = 0;
    size_t end = 0;
    if (!notes_group_bounds(notes.cursor, &start, &end)) {
        return;
    }

    size_t target = notes.cursor;
    if (down) {
        if (notes.cursor + 1U >= end) {
            notes_set_message("bottom of section");
            return;
        }
        target = notes.cursor + 1U;
    } else {
        if (notes.cursor <= start) {
            notes_set_message("top of section");
            return;
        }
        target = notes.cursor - 1U;
    }

    const notes_item_t item = notes.items[notes.cursor];
    notes.items[notes.cursor] = notes.items[target];
    notes.items[target] = item;
    notes.cursor = target;
    if (notes_save() == ESP_OK) {
        notes_set_message("moved");
    }
}

static void notes_page(solar_os_shell_io_t *io, bool down)
{
    const size_t step = notes_list_rows(io) > 0 ? notes_list_rows(io) : 1U;
    if (down) {
        notes.cursor = notes.cursor + step < notes.item_count ?
            notes.cursor + step :
            (notes.item_count > 0 ? notes.item_count - 1U : 0U);
    } else {
        notes.cursor = notes.cursor > step ? notes.cursor - step : 0;
    }
}

static void notes_toggle_selected(void)
{
    if (notes.item_count == 0 || notes.cursor >= notes.item_count) {
        return;
    }
    const uint32_t id = notes.items[notes.cursor].id;
    notes.items[notes.cursor].checked = !notes.items[notes.cursor].checked;
    notes_reorder(id);
    (void)notes_save();
}

static void notes_delete_selected(void)
{
    if (notes.item_count == 0 || notes.cursor >= notes.item_count) {
        return;
    }
    for (size_t i = notes.cursor + 1U; i < notes.item_count; i++) {
        notes.items[i - 1U] = notes.items[i];
    }
    notes.item_count--;
    notes_reorder(0);
    (void)notes_save();
}

static esp_err_t notes_start(solar_os_context_t *ctx)
{
    memset(&notes, 0, sizeof(notes));
    notes.next_id = 1;
    if (!notes_alloc_buffers()) {
        return ESP_ERR_NO_MEM;
    }

    const int argc = solar_os_context_argc(ctx);
    if (argc > 2) {
        notes.error_only = true;
        notes_set_message("usage: notes [file.md]");
        notes_render(ctx);
        return ESP_OK;
    }

    if (!solar_os_storage_is_mounted()) {
        notes.error_only = true;
        notes_set_message("SD card not mounted");
        notes_render(ctx);
        return ESP_OK;
    }

    const char *arg = argc == 2 ? solar_os_context_argv(ctx, 1) : NOTES_DEFAULT_PATH;
    if (argc != 2) {
        (void)notes_ensure_default_dir();
    }
    strlcpy(notes.display_name, arg, sizeof(notes.display_name));

    const esp_err_t path_err = solar_os_storage_resolve_path(arg, notes.path, sizeof(notes.path));
    if (path_err != ESP_OK) {
        notes.error_only = true;
        notes_set_message(path_err == ESP_ERR_INVALID_SIZE ? "path too long" : "invalid path");
        notes_render(ctx);
        return ESP_OK;
    }

    const esp_err_t load_err = notes_load();
    if (load_err != ESP_OK) {
        notes.error_only = true;
    }
    notes.running = !notes.error_only;
    notes_render(ctx);
    return ESP_OK;
}

static void notes_stop(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = notes_io(ctx);
    solar_os_shell_io_set_cursor_visible(io, true);
    notes_free_buffers();
    memset(&notes, 0, sizeof(notes));
}

static bool notes_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL || event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_request_exit(ctx);
        return true;
    }

    if (notes.error_only) {
        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
        }
        return true;
    }

    if (notes.input_mode != NOTES_INPUT_NONE) {
        const bool handled = notes_handle_input(ch);
        notes_render(ctx);
        return handled;
    }

    switch (ch) {
    case SOLAR_OS_KEY_ESCAPE:
    case 'q':
    case 'Q':
        solar_os_context_request_exit(ctx);
        return true;
    case SOLAR_OS_KEY_UP:
        notes_move_up();
        break;
    case SOLAR_OS_KEY_DOWN:
        notes_move_down();
        break;
    case SOLAR_OS_KEY_SHIFT_UP:
        notes_move_selected_item(false);
        break;
    case SOLAR_OS_KEY_SHIFT_DOWN:
        notes_move_selected_item(true);
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        notes_page(notes_io(ctx), false);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        notes_page(notes_io(ctx), true);
        break;
    case SOLAR_OS_KEY_HOME:
        notes.cursor = 0;
        break;
    case SOLAR_OS_KEY_END:
        notes.cursor = notes.item_count > 0 ? notes.item_count - 1U : 0;
        break;
    case ' ':
        notes_toggle_selected();
        break;
    case 'a':
    case 'A':
        notes_start_add();
        break;
    case 'd':
    case 'D':
    case SOLAR_OS_KEY_DELETE:
        notes_delete_selected();
        break;
    case '\r':
    case '\n':
        notes_start_edit();
        break;
    default:
        return true;
    }

    notes_render(ctx);
    return true;
}

const solar_os_app_t solar_os_notes_app = {
    .name = "notes",
    .summary = "Markdown checklist notes",
    .start = notes_start,
    .stop = notes_stop,
    .event = notes_event,
};
