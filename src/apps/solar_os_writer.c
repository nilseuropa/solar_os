#include "solar_os_writer.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_attr.h"
#include "solar_os_clipboard.h"
#include "solar_os_doc.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
#include "solar_os_text_search.h"
#include "solar_os_writer_buffer.h"
#include "solar_os_writer_files.h"

#define WRITER_HEADER_H 16
#define WRITER_MARGIN_X 4
#define WRITER_MARGIN_Y 3
#define WRITER_MAX_ZOOM 4
#define WRITER_IDLE_RECOVERY_MS 2000U
#define WRITER_CURSOR_BLINK_MS 500U
#define WRITER_MESSAGE_MAX 96
#define WRITER_DIALOG_INPUT_MAX SOLAR_OS_STORAGE_PATH_MAX
#define WRITER_STATE_DIR ".writer"
#define WRITER_META_SUFFIX ".meta"
#define WRITER_RECOVERY_SUFFIX ".recovery"

typedef enum {
    WRITER_DIALOG_NONE,
    WRITER_DIALOG_SAVE_AS,
    WRITER_DIALOG_EXIT,
    WRITER_DIALOG_FORMAT,
    WRITER_DIALOG_FIND,
    WRITER_DIALOG_REPLACE_FIND,
    WRITER_DIALOG_REPLACE_WITH,
    WRITER_DIALOG_RECOVERY,
} writer_dialog_t;

typedef struct {
    solar_os_writer_buffer_t buffer;
    solar_os_doc_t doc;
    solar_os_doc_layout_t layout;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char display_name[SOLAR_OS_STORAGE_PATH_MAX];
    char state_dir[SOLAR_OS_STORAGE_PATH_MAX];
    char metadata_path[SOLAR_OS_STORAGE_PATH_MAX];
    char recovery_path[SOLAR_OS_STORAGE_PATH_MAX];
    size_t cursor;
    size_t selection_anchor;
    size_t saved_scroll_anchor;
    int scroll_y;
    int zoom;
    int content_height;
    int layout_width;
    bool loaded;
    bool dirty;
    bool suspended;
    bool parse_pending;
    bool layout_pending;
    bool render_pending;
    bool recovery_written;
    bool recovery_available;
    bool discard_on_stop;
    bool exit_after_save;
    bool cursor_visible;
    uint32_t last_tick_ms;
    uint32_t last_edit_ms;
    uint32_t cursor_blink_ms;
    writer_dialog_t dialog;
    char dialog_input[WRITER_DIALOG_INPUT_MAX];
    size_t dialog_len;
    char replace_find[WRITER_DIALOG_INPUT_MAX];
    solar_os_text_search_state_t search;
    char message[WRITER_MESSAGE_MAX];
} writer_state_t;

static void *writer_state;
#define writer (*(writer_state_t *)writer_state)

static uint32_t writer_hash_bytes(uint32_t hash, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 16777619U;
    }
    return hash;
}

static uint32_t writer_path_hash(const char *path)
{
    const char *key = path != NULL && path[0] != '\0' ? path : "untitled";
    return writer_hash_bytes(2166136261U, key, strlen(key));
}

static bool writer_path_with_suffix(const char *path,
                                    const char *suffix,
                                    char *out,
                                    size_t out_len)
{
    const int written = snprintf(out, out_len, "%s%s", path, suffix);
    return written >= 0 && (size_t)written < out_len;
}

static void writer_set_message(const char *message)
{
    strlcpy(writer.message, message != NULL ? message : "", sizeof(writer.message));
    writer.render_pending = true;
}

static bool writer_selection(size_t *start, size_t *end)
{
    if (writer.selection_anchor == SIZE_MAX || writer.selection_anchor == writer.cursor) {
        return false;
    }
    if (writer.selection_anchor < writer.cursor) {
        *start = writer.selection_anchor;
        *end = writer.cursor;
    } else {
        *start = writer.cursor;
        *end = writer.selection_anchor;
    }
    return true;
}

static void writer_clear_selection(void)
{
    writer.selection_anchor = SIZE_MAX;
}

static void writer_wake_cursor(void)
{
    if (!writer.cursor_visible) {
        writer.render_pending = true;
    }
    writer.cursor_visible = true;
    writer.cursor_blink_ms = writer.last_tick_ms;
}

static esp_err_t writer_prepare_state_paths(void)
{
    esp_err_t ret = solar_os_storage_default_path(WRITER_STATE_DIR,
                                                  writer.state_dir,
                                                  sizeof(writer.state_dir));
    if (ret != ESP_OK) {
        return ret;
    }
    if (mkdir(writer.state_dir, 0775) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    const uint32_t hash = writer_path_hash(writer.path);
    int written = snprintf(writer.metadata_path,
                           sizeof(writer.metadata_path),
                           "%s/%08" PRIx32 WRITER_META_SUFFIX,
                           writer.state_dir,
                           hash);
    if (written < 0 || (size_t)written >= sizeof(writer.metadata_path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    written = snprintf(writer.recovery_path,
                       sizeof(writer.recovery_path),
                       "%s/%08" PRIx32 WRITER_RECOVERY_SUFFIX,
                       writer.state_dir,
                       hash);
    return written >= 0 && (size_t)written < sizeof(writer.recovery_path) ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t writer_read_file(const char *path, char **out, size_t *out_len)
{
    if (path == NULL || out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > SOLAR_OS_WRITER_BUFFER_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    const size_t len = (size_t)st.st_size;
    char *data = solar_os_memory_alloc(len + 1U,
                                       SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                       "writer.file");
    if (data == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    const bool failed = (len > 0 && fread(data, 1, len, file) != len) || fclose(file) != 0;
    if (failed) {
        solar_os_memory_free(data);
        return ESP_FAIL;
    }
    data[len] = '\0';
    *out = data;
    *out_len = len;
    return ESP_OK;
}

static esp_err_t writer_write_synced_file(const char *path, const char *data, size_t len)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    esp_err_t ret = ESP_OK;
    if ((len > 0 && fwrite(data, 1, len, file) != len) ||
        fflush(file) != 0 || fsync(fileno(file)) != 0) {
        ret = ESP_FAIL;
    }
    if (fclose(file) != 0 && ret == ESP_OK) {
        ret = ESP_FAIL;
    }
    return ret;
}

static size_t writer_scroll_anchor(void)
{
    size_t anchor = writer.cursor;
    if (writer.layout.line_count > 0) {
        (void)solar_os_doc_layout_hit_test(&writer.layout, 0, writer.scroll_y, &anchor);
    }
    return anchor;
}

static void writer_save_metadata(void)
{
    if (writer.metadata_path[0] == '\0') {
        return;
    }
    char text[512];
    const int written = snprintf(text,
                                 sizeof(text),
                                 "%u %u %d\n%s\n",
                                 (unsigned)writer.cursor,
                                 (unsigned)writer_scroll_anchor(),
                                 writer.zoom,
                                 writer.path);
    if (written <= 0 || (size_t)written >= sizeof(text)) {
        return;
    }
    char temp[SOLAR_OS_STORAGE_PATH_MAX];
    if (!writer_path_with_suffix(writer.metadata_path, ".tmp", temp, sizeof(temp))) {
        return;
    }
    if (writer_write_synced_file(temp, text, (size_t)written) == ESP_OK) {
        (void)rename(temp, writer.metadata_path);
    } else {
        (void)remove(temp);
    }
}

static void writer_load_metadata(void)
{
    FILE *file = fopen(writer.metadata_path, "rb");
    if (file == NULL) {
        return;
    }
    unsigned cursor = 0;
    unsigned anchor = 0;
    int zoom = 1;
    char path[SOLAR_OS_STORAGE_PATH_MAX] = {0};
    bool valid = fscanf(file, "%u %u %d\n", &cursor, &anchor, &zoom) == 3 &&
        fgets(path, sizeof(path), file) != NULL;
    fclose(file);
    if (!valid) {
        return;
    }
    path[strcspn(path, "\r\n")] = '\0';
    if (strcmp(path, writer.path) != 0 || zoom < 0 || zoom > WRITER_MAX_ZOOM) {
        return;
    }
    const size_t len = solar_os_writer_buffer_length(&writer.buffer);
    writer.cursor = cursor <= len ? cursor : len;
    writer.saved_scroll_anchor = anchor <= len ? anchor : writer.cursor;
    writer.zoom = zoom;
}

static esp_err_t writer_write_recovery(void)
{
    if (!writer.dirty || writer.recovery_path[0] == '\0') {
        return ESP_OK;
    }
    char *flat = NULL;
    size_t len = 0;
    esp_err_t ret = solar_os_writer_buffer_flatten(&writer.buffer, &flat, &len);
    if (ret == ESP_OK) {
        char temp[SOLAR_OS_STORAGE_PATH_MAX];
        if (!writer_path_with_suffix(writer.recovery_path, ".tmp", temp, sizeof(temp))) {
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            ret = writer_write_synced_file(temp, flat, len);
            if (ret == ESP_OK && rename(temp, writer.recovery_path) != 0) {
                ret = ESP_FAIL;
            }
            if (ret != ESP_OK) {
                (void)remove(temp);
            }
        }
    }
    solar_os_memory_free(flat);
    if (ret == ESP_OK) {
        writer.recovery_written = true;
        writer_save_metadata();
    }
    return ret;
}

static bool writer_recovery_differs(void)
{
    struct stat recovery_stat;
    if (stat(writer.recovery_path, &recovery_stat) != 0 ||
        recovery_stat.st_size < 0 ||
        (uint64_t)recovery_stat.st_size > SOLAR_OS_WRITER_BUFFER_MAX_BYTES) {
        return false;
    }
    if (writer.path[0] != '\0') {
        struct stat saved_stat;
        if (stat(writer.path, &saved_stat) == 0 && recovery_stat.st_mtime > saved_stat.st_mtime) {
            return true;
        }
    }
    char *snapshot = NULL;
    size_t snapshot_len = 0;
    if (writer_read_file(writer.recovery_path, &snapshot, &snapshot_len) != ESP_OK) {
        return false;
    }
    char *current = NULL;
    size_t current_len = 0;
    const bool flat_ok = solar_os_writer_buffer_flatten(&writer.buffer,
                                                        &current,
                                                        &current_len) == ESP_OK;
    const bool differs = !flat_ok || snapshot_len != current_len ||
        (snapshot_len > 0 && memcmp(snapshot, current, snapshot_len) != 0);
    solar_os_memory_free(snapshot);
    solar_os_memory_free(current);
    return differs;
}

static void writer_mark_changed(void)
{
    writer.dirty = true;
    writer.parse_pending = true;
    writer.layout_pending = true;
    writer.render_pending = true;
    writer.recovery_written = false;
    writer.last_edit_ms = writer.last_tick_ms;
    writer.message[0] = '\0';
}

static esp_err_t writer_replace_range(size_t start,
                                      size_t end,
                                      const char *text,
                                      size_t text_len,
                                      size_t cursor_after)
{
    if (end < start) {
        const size_t swap = start;
        start = end;
        end = swap;
    }
    const size_t len = solar_os_writer_buffer_length(&writer.buffer);
    if (start > len || end > len) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_writer_buffer_replace(&writer.buffer,
                                                   start,
                                                   end - start,
                                                   text,
                                                   text_len,
                                                   writer.cursor,
                                                   cursor_after);
    if (ret == ESP_OK) {
        writer.cursor = cursor_after;
        writer_clear_selection();
        writer_mark_changed();
    } else if (ret == ESP_ERR_INVALID_SIZE) {
        writer_set_message("document limit is 256 KiB; use edit or reader");
    } else {
        writer_set_message("edit failed");
    }
    return ret;
}

static esp_err_t writer_insert(const char *text, size_t text_len)
{
    size_t start = writer.cursor;
    size_t end = writer.cursor;
    (void)writer_selection(&start, &end);
    return writer_replace_range(start, end, text, text_len, start + text_len);
}

static bool writer_byte(size_t offset, char *ch)
{
    return ch != NULL &&
        solar_os_writer_buffer_copy(&writer.buffer, offset, 1, ch) == ESP_OK;
}

static size_t writer_line_start(size_t offset)
{
    while (offset > 0) {
        char ch = '\0';
        if (!writer_byte(offset - 1U, &ch) || ch == '\n' || ch == '\r') {
            break;
        }
        offset--;
    }
    return offset;
}

static size_t writer_line_end(size_t offset)
{
    const size_t len = solar_os_writer_buffer_length(&writer.buffer);
    while (offset < len) {
        char ch = '\0';
        if (!writer_byte(offset, &ch) || ch == '\n' || ch == '\r') {
            break;
        }
        offset++;
    }
    return offset;
}

static void writer_set_cursor(size_t cursor, bool select)
{
    const size_t len = solar_os_writer_buffer_length(&writer.buffer);
    if (cursor > len) {
        cursor = len;
    }
    if (select) {
        if (writer.selection_anchor == SIZE_MAX) {
            writer.selection_anchor = writer.cursor;
        }
    } else {
        writer_clear_selection();
    }
    writer.cursor = cursor;
    writer_wake_cursor();
    writer.layout_pending = true;
    writer.render_pending = true;
}

static void writer_move_vertical(int delta_lines, bool select)
{
    if (delta_lines == 0) {
        return;
    }
    size_t target = writer.cursor;
    if (solar_os_doc_layout_adjacent_source(&writer.layout,
                                            writer.cursor,
                                            delta_lines > 0,
                                            &target)) {
        writer_set_cursor(target, select);
    }
}

static int writer_view_height(solar_os_gfx_t *gfx)
{
    int height = (int)solar_os_gfx_height(gfx) - WRITER_HEADER_H - (2 * WRITER_MARGIN_Y);
    return height > 8 ? height : 8;
}

static void writer_page(solar_os_context_t *ctx, bool down, bool select)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    int x = 0;
    int y = 0;
    int height = 14;
    if (!solar_os_doc_layout_source_to_xy(&writer.layout,
                                          writer.cursor,
                                          &x,
                                          &y,
                                          &height)) {
        return;
    }
    int step = writer_view_height(gfx) - height;
    if (step < height) {
        step = height;
    }
    size_t target = writer.cursor;
    (void)solar_os_doc_layout_hit_test(&writer.layout,
                                      x,
                                      down ? y + step : y - step,
                                      &target);
    writer_set_cursor(target, select);
}

static void writer_delete(bool forward)
{
    size_t start = writer.cursor;
    size_t end = writer.cursor;
    if (!writer_selection(&start, &end)) {
        if (forward) {
            end = solar_os_writer_utf8_next(&writer.buffer, writer.cursor);
        } else {
            start = solar_os_writer_utf8_prev(&writer.buffer, writer.cursor);
        }
    }
    if (end > start) {
        (void)writer_replace_range(start, end, NULL, 0, start);
    }
}

static void writer_copy_selection(bool cut)
{
    size_t start = 0;
    size_t end = 0;
    if (!writer_selection(&start, &end)) {
        writer_set_message("no selection");
        return;
    }
    char *copy = solar_os_memory_alloc(end - start,
                                       SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                       "writer.clip");
    if (copy == NULL) {
        writer_set_message("clipboard allocation failed");
        return;
    }
    if (solar_os_writer_buffer_copy(&writer.buffer, start, end - start, copy) != ESP_OK ||
        solar_os_clipboard_set(copy, end - start) != ESP_OK) {
        writer_set_message("clipboard failed");
    } else if (cut) {
        (void)writer_replace_range(start, end, NULL, 0, start);
    } else {
        writer_set_message("copied");
    }
    solar_os_memory_free(copy);
}

static void writer_paste(void)
{
    size_t len = 0;
    const char *text = solar_os_clipboard_data(&len);
    if (text == NULL || len == 0) {
        writer_set_message("clipboard empty");
        return;
    }
    (void)writer_insert(text, len);
}

static bool writer_range_matches(size_t offset, const char *text, size_t len)
{
    if (text == NULL || offset > solar_os_writer_buffer_length(&writer.buffer) ||
        len > solar_os_writer_buffer_length(&writer.buffer) - offset) {
        return false;
    }
    char local[8];
    if (len > sizeof(local)) {
        return false;
    }
    return solar_os_writer_buffer_copy(&writer.buffer, offset, len, local) == ESP_OK &&
        memcmp(local, text, len) == 0;
}

static void writer_wrap(const char *open, const char *close)
{
    const size_t open_len = strlen(open);
    const size_t close_len = strlen(close);
    size_t start = writer.cursor;
    size_t end = writer.cursor;
    const bool selected = writer_selection(&start, &end);
    if (selected && start >= open_len &&
        writer_range_matches(start - open_len, open, open_len) &&
        writer_range_matches(end, close, close_len)) {
        const size_t content_len = end - start;
        char *content = solar_os_memory_alloc(content_len,
                                              SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                              "writer.format");
        if (content == NULL && content_len > 0) {
            writer_set_message("format allocation failed");
            return;
        }
        (void)solar_os_writer_buffer_copy(&writer.buffer, start, content_len, content);
        (void)writer_replace_range(start - open_len,
                                   end + close_len,
                                   content,
                                   content_len,
                                   start - open_len + content_len);
        solar_os_memory_free(content);
        return;
    }

    const size_t content_len = selected ? end - start : 0;
    const size_t total = open_len + content_len + close_len;
    char *replacement = solar_os_memory_alloc(total,
                                              SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                              "writer.format");
    if (replacement == NULL) {
        writer_set_message("format allocation failed");
        return;
    }
    memcpy(replacement, open, open_len);
    if (content_len > 0) {
        (void)solar_os_writer_buffer_copy(&writer.buffer,
                                          start,
                                          content_len,
                                          &replacement[open_len]);
    }
    memcpy(&replacement[open_len + content_len], close, close_len);
    const size_t cursor = selected ? start + total : start + open_len;
    (void)writer_replace_range(start, end, replacement, total, cursor);
    solar_os_memory_free(replacement);
}

static void writer_prefix_lines(const char *prefix)
{
    size_t start = writer.cursor;
    size_t end = writer.cursor;
    (void)writer_selection(&start, &end);
    start = writer_line_start(start);
    end = writer_line_end(end);
    const size_t content_len = end - start;
    const size_t prefix_len = strlen(prefix);
    char *content = solar_os_memory_alloc(content_len + 1U,
                                          SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                          "writer.format");
    if (content == NULL) {
        writer_set_message("format allocation failed");
        return;
    }
    (void)solar_os_writer_buffer_copy(&writer.buffer, start, content_len, content);
    content[content_len] = '\0';

    size_t lines = 1;
    for (size_t i = 0; i < content_len; i++) {
        if (content[i] == '\n') {
            lines++;
        }
    }
    bool remove_prefix = true;
    for (size_t i = 0; i < content_len;) {
        if (i + prefix_len > content_len || memcmp(&content[i], prefix, prefix_len) != 0) {
            remove_prefix = false;
            break;
        }
        const char *newline = memchr(&content[i], '\n', content_len - i);
        if (newline == NULL) {
            break;
        }
        i = (size_t)(newline - content) + 1U;
        if (i == content_len) {
            break;
        }
    }
    const size_t output_cap = content_len + (lines * prefix_len) + 1U;
    char *output = solar_os_memory_alloc(output_cap,
                                         SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                         "writer.format");
    if (output == NULL) {
        solar_os_memory_free(content);
        writer_set_message("format allocation failed");
        return;
    }
    size_t out_len = 0;
    bool at_line_start = true;
    for (size_t i = 0; i < content_len; i++) {
        if (at_line_start) {
            if (remove_prefix) {
                i += prefix_len;
                if (i >= content_len) {
                    break;
                }
            } else {
                memcpy(&output[out_len], prefix, prefix_len);
                out_len += prefix_len;
            }
            at_line_start = false;
        }
        output[out_len++] = content[i];
        if (content[i] == '\n') {
            at_line_start = true;
        }
    }
    if (content_len == 0) {
        memcpy(output, prefix, prefix_len);
        out_len = prefix_len;
    }
    (void)writer_replace_range(start, end, output, out_len, start + out_len);
    solar_os_memory_free(output);
    solar_os_memory_free(content);
}

static void writer_link(void)
{
    size_t start = writer.cursor;
    size_t end = writer.cursor;
    const bool selected = writer_selection(&start, &end);
    const bool remove_existing = selected && start > 0 &&
        writer_range_matches(start - 1U, "[", 1) &&
        writer_range_matches(end, "]()", 3);
    writer_wrap("[", "]()");
    if (selected && !remove_existing) {
        writer.cursor = start + (end - start) + 3U;
        writer.layout_pending = true;
        writer.render_pending = true;
    }
}

static void writer_heading(unsigned level)
{
    const size_t start = writer_line_start(writer.cursor);
    const size_t end = writer_line_end(writer.cursor);
    size_t marker_end = start;
    char ch = '\0';
    while (marker_end < end && writer_byte(marker_end, &ch) && ch == '#') {
        marker_end++;
    }
    if (marker_end > start && marker_end < end && writer_byte(marker_end, &ch) && ch == ' ') {
        marker_end++;
    } else {
        marker_end = start;
    }
    char prefix[6];
    memset(prefix, '#', level);
    prefix[level] = ' ';
    prefix[level + 1U] = '\0';
    const bool remove_existing = marker_end - start == level + 1U;
    const size_t replacement_len = remove_existing ? 0 : level + 1U;
    const size_t cursor_after = writer.cursor >= marker_end ?
        writer.cursor - (marker_end - start) + replacement_len :
        start + replacement_len;
    (void)writer_replace_range(start,
                               marker_end,
                               remove_existing ? NULL : prefix,
                               replacement_len,
                               cursor_after);
}

static void writer_apply_format(uint8_t key)
{
    switch (key) {
    case '1': writer_wrap("`", "`"); break;
    case '2': writer_heading(1); break;
    case '3': writer_heading(2); break;
    case '4': writer_heading(3); break;
    case '5': writer_heading(4); break;
    case '6': writer_prefix_lines("- "); break;
    case '7': writer_prefix_lines("1. "); break;
    case '8': writer_prefix_lines("> "); break;
    case '9': writer_wrap("```\n", "\n```"); break;
    case '0': (void)writer_insert("\n---\n", 5); break;
    default: return;
    }
    writer.dialog = WRITER_DIALOG_NONE;
}

static void writer_begin_dialog(writer_dialog_t dialog, const char *initial)
{
    writer.dialog = dialog;
    strlcpy(writer.dialog_input, initial != NULL ? initial : "", sizeof(writer.dialog_input));
    writer.dialog_len = strlen(writer.dialog_input);
    writer.render_pending = true;
}

static bool writer_find_from(const char *query, size_t *match_start)
{
    if (query == NULL || query[0] == '\0') {
        return false;
    }
    char *flat = NULL;
    size_t len = 0;
    if (solar_os_writer_buffer_flatten(&writer.buffer, &flat, &len) != ESP_OK) {
        return false;
    }
    solar_os_text_search_match_t match;
    const bool found = solar_os_text_search_find(flat,
                                                 len,
                                                 query,
                                                 writer.cursor,
                                                 false,
                                                 SOLAR_OS_TEXT_SEARCH_FORWARD,
                                                 &match);
    if (found) {
        *match_start = match.offset;
    }
    solar_os_memory_free(flat);
    return found;
}

static void writer_submit_find(const char *query)
{
    if (!solar_os_text_search_set_query(&writer.search, query)) {
        writer_set_message("empty search");
        writer.dialog = WRITER_DIALOG_NONE;
        return;
    }
    size_t match = 0;
    if (!writer_find_from(query, &match)) {
        writer_set_message("not found");
        writer.dialog = WRITER_DIALOG_NONE;
        return;
    }
    writer.selection_anchor = match;
    writer.cursor = match + strlen(query);
    writer.search.match.offset = match;
    writer.search.match.length = strlen(query);
    writer.search.match_valid = true;
    writer.layout_pending = true;
    writer.render_pending = true;
    writer.dialog = WRITER_DIALOG_NONE;
    writer_set_message("found");
}

static esp_err_t writer_save_to(const char *target)
{
    char resolved[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t ret = solar_os_storage_resolve_path(target, resolved, sizeof(resolved));
    if (ret != ESP_OK) {
        writer_set_message(ret == ESP_ERR_INVALID_SIZE ? "path too long" : "invalid path");
        return ret;
    }
    char *flat = NULL;
    size_t len = 0;
    ret = solar_os_writer_buffer_flatten(&writer.buffer, &flat, &len);
    if (ret == ESP_OK) {
        ret = solar_os_writer_safe_replace(resolved,
                                           flat,
                                           len,
                                           SOLAR_OS_WRITER_FILE_FAULT_NONE);
    }
    solar_os_memory_free(flat);
    if (ret != ESP_OK) {
        writer_set_message("save failed; original restored");
        return ret;
    }

    char old_recovery[SOLAR_OS_STORAGE_PATH_MAX];
    strlcpy(old_recovery, writer.recovery_path, sizeof(old_recovery));
    strlcpy(writer.path, resolved, sizeof(writer.path));
    strlcpy(writer.display_name, target, sizeof(writer.display_name));
    writer.dirty = false;
    writer.recovery_written = false;
    (void)writer_prepare_state_paths();
    (void)remove(old_recovery);
    (void)remove(writer.recovery_path);
    writer_save_metadata();
    writer_set_message("saved");
    return ESP_OK;
}

static void writer_request_save(void)
{
    writer.exit_after_save = false;
    if (writer.path[0] == '\0') {
        writer_begin_dialog(WRITER_DIALOG_SAVE_AS, "");
    } else {
        (void)writer_save_to(writer.path);
    }
}

static void writer_request_exit(solar_os_context_t *ctx)
{
    if (!writer.dirty) {
        solar_os_context_finish(ctx, 0, NULL);
        return;
    }
    writer.dialog = WRITER_DIALOG_EXIT;
    writer.render_pending = true;
}

static bool writer_handle_dialog(solar_os_context_t *ctx, uint8_t ch)
{
    if (writer.dialog == WRITER_DIALOG_FORMAT) {
        if (ch == SOLAR_OS_KEY_ESCAPE) {
            writer.dialog = WRITER_DIALOG_NONE;
        } else {
            writer_apply_format(ch);
        }
        writer.render_pending = true;
        return true;
    }
    if (writer.dialog == WRITER_DIALOG_EXIT) {
        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'c' || ch == 'C') {
            writer.dialog = WRITER_DIALOG_NONE;
        } else if (ch == 'd' || ch == 'D') {
            writer.discard_on_stop = true;
            (void)remove(writer.recovery_path);
            solar_os_context_finish(ctx, 0, NULL);
        } else if (ch == 's' || ch == 'S') {
            if (writer.path[0] == '\0') {
                writer.exit_after_save = true;
                writer_begin_dialog(WRITER_DIALOG_SAVE_AS, "");
            } else if (writer_save_to(writer.path) == ESP_OK) {
                solar_os_context_finish(ctx, 0, NULL);
            }
        }
        writer.render_pending = true;
        return true;
    }
    if (writer.dialog == WRITER_DIALOG_RECOVERY) {
        if (ch == 'r' || ch == 'R' || ch == 'y' || ch == 'Y') {
            char *snapshot = NULL;
            size_t len = 0;
            if (writer_read_file(writer.recovery_path, &snapshot, &len) == ESP_OK &&
                solar_os_writer_buffer_set(&writer.buffer, snapshot, len) == ESP_OK) {
                writer.cursor = writer.cursor <= len ? writer.cursor : len;
                writer.dirty = true;
                writer.recovery_written = true;
                writer.parse_pending = true;
                writer.layout_pending = true;
                writer_set_message("recovered");
            } else {
                writer_set_message("recovery failed");
            }
            solar_os_memory_free(snapshot);
            writer.dialog = WRITER_DIALOG_NONE;
        } else if (ch == 'n' || ch == 'N' || ch == 'd' || ch == 'D') {
            (void)remove(writer.recovery_path);
            writer.recovery_available = false;
            writer.dialog = WRITER_DIALOG_NONE;
            writer_set_message("recovery discarded");
        } else if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'c' || ch == 'C') {
            writer.dialog = WRITER_DIALOG_NONE;
        }
        writer.render_pending = true;
        return true;
    }

    if (ch == SOLAR_OS_KEY_ESCAPE) {
        if (writer.dialog == WRITER_DIALOG_SAVE_AS) {
            writer.exit_after_save = false;
        }
        writer.dialog = WRITER_DIALOG_NONE;
        writer.dialog_len = 0;
        writer.dialog_input[0] = '\0';
        writer.render_pending = true;
        return true;
    }
    if (ch == '\b' || ch == 0x7fU) {
        if (writer.dialog_len > 0) {
            writer.dialog_input[--writer.dialog_len] = '\0';
        }
        writer.render_pending = true;
        return true;
    }
    if (ch == '\r' || ch == '\n') {
        writer_dialog_t submitted = writer.dialog;
        writer.dialog = WRITER_DIALOG_NONE;
        if (submitted == WRITER_DIALOG_SAVE_AS) {
            if (writer.dialog_len == 0) {
                writer_set_message("enter a path");
                writer.dialog = WRITER_DIALOG_SAVE_AS;
            } else if (writer_save_to(writer.dialog_input) == ESP_OK && writer.exit_after_save) {
                solar_os_context_finish(ctx, 0, NULL);
            }
        } else if (submitted == WRITER_DIALOG_FIND) {
            writer_submit_find(writer.dialog_input);
        } else if (submitted == WRITER_DIALOG_REPLACE_FIND) {
            strlcpy(writer.replace_find, writer.dialog_input, sizeof(writer.replace_find));
            writer_begin_dialog(WRITER_DIALOG_REPLACE_WITH, "");
        } else if (submitted == WRITER_DIALOG_REPLACE_WITH) {
            size_t match = 0;
            if (writer_find_from(writer.replace_find, &match)) {
                (void)writer_replace_range(match,
                                           match + strlen(writer.replace_find),
                                           writer.dialog_input,
                                           writer.dialog_len,
                                           match + writer.dialog_len);
                writer_set_message("replaced");
            } else {
                writer_set_message("not found");
            }
        }
        writer.render_pending = true;
        return true;
    }
    if (ch >= 0x20U && ch < 0x80U && writer.dialog_len + 1U < sizeof(writer.dialog_input)) {
        writer.dialog_input[writer.dialog_len++] = (char)ch;
        writer.dialog_input[writer.dialog_len] = '\0';
        writer.render_pending = true;
    }
    return true;
}

static bool writer_handle_char(solar_os_context_t *ctx, uint8_t ch)
{
    writer_wake_cursor();
    if (writer.dialog != WRITER_DIALOG_NONE) {
        return writer_handle_dialog(ctx, ch);
    }
    if (ch == SOLAR_OS_KEY_APP_EXIT) {
        writer_request_exit(ctx);
        return true;
    }
    switch (ch) {
    case SOLAR_OS_KEY_ESCAPE:
        writer_request_exit(ctx);
        break;
    case SOLAR_OS_KEY_LEFT:
        writer_set_cursor(solar_os_writer_utf8_prev(&writer.buffer, writer.cursor), false);
        break;
    case SOLAR_OS_KEY_RIGHT:
        writer_set_cursor(solar_os_writer_utf8_next(&writer.buffer, writer.cursor), false);
        break;
    case SOLAR_OS_KEY_SHIFT_LEFT:
        writer_set_cursor(solar_os_writer_utf8_prev(&writer.buffer, writer.cursor), true);
        break;
    case SOLAR_OS_KEY_SHIFT_RIGHT:
        writer_set_cursor(solar_os_writer_utf8_next(&writer.buffer, writer.cursor), true);
        break;
    case SOLAR_OS_KEY_CTRL_LEFT:
        writer_set_cursor(solar_os_writer_word_prev(&writer.buffer, writer.cursor), false);
        break;
    case SOLAR_OS_KEY_CTRL_RIGHT:
        writer_set_cursor(solar_os_writer_word_next(&writer.buffer, writer.cursor), false);
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_LEFT:
        writer_set_cursor(solar_os_writer_word_prev(&writer.buffer, writer.cursor), true);
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_RIGHT:
        writer_set_cursor(solar_os_writer_word_next(&writer.buffer, writer.cursor), true);
        break;
    case SOLAR_OS_KEY_UP: writer_move_vertical(-1, false); break;
    case SOLAR_OS_KEY_DOWN: writer_move_vertical(1, false); break;
    case SOLAR_OS_KEY_SHIFT_UP: writer_move_vertical(-1, true); break;
    case SOLAR_OS_KEY_SHIFT_DOWN: writer_move_vertical(1, true); break;
    case SOLAR_OS_KEY_PAGE_UP: writer_page(ctx, false, false); break;
    case SOLAR_OS_KEY_PAGE_DOWN: writer_page(ctx, true, false); break;
    case SOLAR_OS_KEY_SHIFT_PAGE_UP: writer_page(ctx, false, true); break;
    case SOLAR_OS_KEY_SHIFT_PAGE_DOWN: writer_page(ctx, true, true); break;
    case SOLAR_OS_KEY_HOME: writer_set_cursor(writer_line_start(writer.cursor), false); break;
    case SOLAR_OS_KEY_END: writer_set_cursor(writer_line_end(writer.cursor), false); break;
    case SOLAR_OS_KEY_SHIFT_HOME: writer_set_cursor(writer_line_start(writer.cursor), true); break;
    case SOLAR_OS_KEY_SHIFT_END: writer_set_cursor(writer_line_end(writer.cursor), true); break;
    case SOLAR_OS_KEY_CTRL_HOME: writer_set_cursor(0, false); break;
    case SOLAR_OS_KEY_CTRL_END:
        writer_set_cursor(solar_os_writer_buffer_length(&writer.buffer), false);
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_HOME: writer_set_cursor(0, true); break;
    case SOLAR_OS_KEY_CTRL_SHIFT_END:
        writer_set_cursor(solar_os_writer_buffer_length(&writer.buffer), true);
        break;
    case SOLAR_OS_KEY_DELETE: writer_delete(true); break;
    case '\b':
    case 0x7fU: writer_delete(false); break;
    case '\r':
    case '\n': (void)writer_insert("\n", 1); break;
    case 0x01U:
        writer.selection_anchor = 0;
        writer.cursor = solar_os_writer_buffer_length(&writer.buffer);
        writer.layout_pending = true;
        writer.render_pending = true;
        break;
    case 0x02U: writer_wrap("**", "**"); break;
    case 0x03U: writer_copy_selection(false); break;
    case 0x06U: writer_begin_dialog(WRITER_DIALOG_FIND, writer.search.query); break;
    case SOLAR_OS_KEY_F3:
        if (writer.search.query_len > 0U) {
            writer_submit_find(writer.search.query);
        } else {
            writer_set_message("no search");
        }
        break;
    case 0x09U: writer_wrap("*", "*"); break;
    case 0x0bU: writer_link(); break;
    case 0x12U: writer_begin_dialog(WRITER_DIALOG_REPLACE_FIND, ""); break;
    case 0x13U: writer_request_save(); break;
    case 0x16U: writer_paste(); break;
    case 0x18U: writer_copy_selection(true); break;
    case 0x19U:
        if (solar_os_writer_buffer_redo(&writer.buffer, &writer.cursor)) {
            writer_clear_selection();
            writer_mark_changed();
        }
        break;
    case 0x1aU:
        if (solar_os_writer_buffer_undo(&writer.buffer, &writer.cursor)) {
            writer_clear_selection();
            writer_mark_changed();
        }
        break;
    case SOLAR_OS_KEY_F1:
        writer.dialog = WRITER_DIALOG_FORMAT;
        writer.render_pending = true;
        break;
    case SOLAR_OS_KEY_CTRL_PLUS:
        if (writer.zoom < WRITER_MAX_ZOOM) {
            writer.zoom++;
            writer.layout_pending = true;
            writer.render_pending = true;
        }
        break;
    case SOLAR_OS_KEY_CTRL_MINUS:
        if (writer.zoom > 0) {
            writer.zoom--;
            writer.layout_pending = true;
            writer.render_pending = true;
        }
        break;
    default:
        if (ch >= 0x20U && ch != 0x7fU) {
            const char value = (char)ch;
            (void)writer_insert(&value, 1);
        }
        break;
    }
    return true;
}

static int writer_max_scroll(solar_os_gfx_t *gfx)
{
    const int max_scroll = writer.content_height - writer_view_height(gfx);
    return max_scroll > 0 ? max_scroll : 0;
}

static void writer_ensure_cursor_visible(solar_os_gfx_t *gfx)
{
    int x = 0;
    int y = 0;
    int height = 14;
    if (!solar_os_doc_layout_source_to_xy(&writer.layout,
                                          writer.cursor,
                                          &x,
                                          &y,
                                          &height)) {
        return;
    }
    const int view_h = writer_view_height(gfx);
    if (y < writer.scroll_y) {
        writer.scroll_y = y;
    } else if (y + height > writer.scroll_y + view_h) {
        writer.scroll_y = y + height - view_h;
    }
    if (writer.scroll_y < 0) {
        writer.scroll_y = 0;
    }
    const int max_scroll = writer_max_scroll(gfx);
    if (writer.scroll_y > max_scroll) {
        writer.scroll_y = max_scroll;
    }
}

static esp_err_t writer_update_document(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (writer.parse_pending) {
        char *flat = NULL;
        size_t len = 0;
        esp_err_t ret = solar_os_writer_buffer_flatten(&writer.buffer, &flat, &len);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = solar_os_doc_parse_markdown(&writer.doc,
                                          flat,
                                          len,
                                          writer.path[0] != '\0' ? writer.path : "untitled.md");
        solar_os_memory_free(flat);
        if (ret != ESP_OK) {
            return ret;
        }
        writer.parse_pending = false;
        writer.layout_pending = true;
    }
    if (writer.layout_pending) {
        const int width = (int)solar_os_gfx_width(gfx) - (2 * WRITER_MARGIN_X) - 4;
        solar_os_doc_reveal_range_t reveal[1];
        size_t reveal_count = 1;
        size_t start = writer.cursor;
        size_t end = writer.cursor;
        if (writer_selection(&start, &end)) {
            reveal[0] = (solar_os_doc_reveal_range_t){.start = start, .end = end};
        } else {
            reveal[0] = (solar_os_doc_reveal_range_t){.start = writer.cursor, .end = writer.cursor};
        }
        esp_err_t ret = solar_os_doc_layout_build_ex(&writer.layout,
                                                     &writer.doc,
                                                     width > 8 ? width : 8,
                                                     writer.zoom,
                                                     reveal,
                                                     reveal_count);
        if (ret != ESP_OK) {
            return ret;
        }
        writer.layout_width = width;
        writer.content_height = writer.layout.height;
        writer.layout_pending = false;
        if (writer.saved_scroll_anchor != SIZE_MAX) {
            int anchor_y = 0;
            if (solar_os_doc_layout_source_to_xy(&writer.layout,
                                                 writer.saved_scroll_anchor,
                                                 NULL,
                                                 &anchor_y,
                                                 NULL)) {
                writer.scroll_y = anchor_y;
            }
            writer.saved_scroll_anchor = SIZE_MAX;
        }
        writer_ensure_cursor_visible(gfx);
    }
    return ESP_OK;
}

static void writer_draw_highlight(solar_os_gfx_t *gfx, const solar_os_doc_view_t *view)
{
    size_t start = 0;
    size_t end = 0;
    if (!writer_selection(&start, &end)) {
        return;
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
    for (size_t i = 0; i < writer.layout.run_count; i++) {
        const solar_os_doc_layout_run_t *run = &writer.layout.runs[i];
        if (run->source_end <= start || run->source_start >= end || run->text_len == 0) {
            continue;
        }
        size_t from = start > run->source_start ? start - run->source_start : 0;
        size_t to = end < run->source_end ? end - run->source_start : run->text_len;
        if (from > run->text_len) {
            from = run->text_len;
        }
        if (to > run->text_len) {
            to = run->text_len;
        }
        if (to <= from) {
            continue;
        }
        const int x = view->x + run->x + ((int)from * run->char_w);
        const int y = view->y + run->y - view->scroll_y;
        solar_os_gfx_fill_rect(gfx, x, y, (int)(to - from) * run->char_w, run->height);
    }
}

static void writer_draw_header(solar_os_gfx_t *gfx)
{
    const int width = (int)solar_os_gfx_width(gfx);
    char title[192];
    snprintf(title,
             sizeof(title),
             "writer%s z%d %s%s",
             writer.dirty ? "*" : "",
             writer.zoom,
             writer.display_name[0] != '\0' ? writer.display_name : "untitled.md",
             writer.message[0] != '\0' ? " - " : "");
    if (writer.message[0] != '\0') {
        strlcat(title, writer.message, sizeof(title));
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, width, WRITER_HEADER_H);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    title[(width / 6) < (int)sizeof(title) ? (size_t)(width / 6) : sizeof(title) - 1U] = '\0';
    solar_os_gfx_text(gfx, 3, 11, title);
}

static void writer_draw_dialog_action(solar_os_gfx_t *gfx,
                                      int x,
                                      int baseline_y,
                                      char key,
                                      const char *label)
{
    char key_symbol[] = {'[', key, ']', '\0'};
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_14);
    solar_os_gfx_text(gfx, x, baseline_y, key_symbol);
    const int label_x = x + (int)solar_os_gfx_text_width(gfx, key_symbol);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_14);
    solar_os_gfx_text(gfx, label_x, baseline_y, label);
}

static size_t writer_dialog_action_width(solar_os_gfx_t *gfx,
                                         char key,
                                         const char *label)
{
    char key_symbol[] = {'[', key, ']', '\0'};
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_14);
    const size_t key_width = solar_os_gfx_text_width(gfx, key_symbol);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_14);
    return key_width + solar_os_gfx_text_width(gfx, label);
}

static void writer_draw_dialog(solar_os_gfx_t *gfx)
{
    if (writer.dialog == WRITER_DIALOG_NONE) {
        return;
    }
    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);
    const bool choice_menu = writer.dialog == WRITER_DIALOG_EXIT ||
        writer.dialog == WRITER_DIALOG_RECOVERY;
    const bool recovery = writer.dialog == WRITER_DIALOG_RECOVERY;
    int x = 12;
    int y = screen_h / 3;
    int w = screen_w - 24;
    int h = writer.dialog == WRITER_DIALOG_FORMAT ? 78 : 48;
    const char *choice_title = recovery ? "Recovery found" : "Unsaved changes";
    if (choice_menu) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_14);
        size_t content_width = solar_os_gfx_text_width(gfx, choice_title);
        const size_t first_width = writer_dialog_action_width(gfx,
                                                               recovery ? 'R' : 'S',
                                                               recovery ? " Recover" : " Save");
        const size_t discard_width = writer_dialog_action_width(gfx, 'D', " Discard");
        const size_t cancel_width = writer_dialog_action_width(gfx, 'C', " Cancel");
        if (first_width > content_width) {
            content_width = first_width;
        }
        if (discard_width > content_width) {
            content_width = discard_width;
        }
        if (cancel_width > content_width) {
            content_width = cancel_width;
        }
        w = (int)content_width + 16;
        h = 84;
        x = (screen_w - w) / 2;
        y = (screen_h - h) / 2;
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, w, h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
    solar_os_gfx_rect(gfx, x, y, w, h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);

    if (choice_menu) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_14);
        solar_os_gfx_text(gfx,
                          x + 8,
                          y + 18,
                          choice_title);
        writer_draw_dialog_action(gfx,
                                  x + 8,
                                  y + 42,
                                  recovery ? 'R' : 'S',
                                  recovery ? " Recover" : " Save");
        writer_draw_dialog_action(gfx, x + 8, y + 58, 'D', " Discard");
        writer_draw_dialog_action(gfx, x + 8, y + 74, 'C', " Cancel");
        return;
    }

    const char *prompt = "";
    switch (writer.dialog) {
    case WRITER_DIALOG_SAVE_AS: prompt = "Save as:"; break;
    case WRITER_DIALOG_EXIT: break;
    case WRITER_DIALOG_FORMAT:
        solar_os_gfx_text(gfx, x + 5, y + 14, "1 code  2-5 heading 1-4");
        solar_os_gfx_text(gfx, x + 5, y + 30, "6 bullet  7 numbered  8 quote");
        solar_os_gfx_text(gfx, x + 5, y + 46, "9 fenced code  0 rule");
        solar_os_gfx_text(gfx, x + 5, y + 64, "Esc closes");
        return;
    case WRITER_DIALOG_FIND: prompt = "Find:"; break;
    case WRITER_DIALOG_REPLACE_FIND: prompt = "Replace find:"; break;
    case WRITER_DIALOG_REPLACE_WITH: prompt = "Replace with:"; break;
    case WRITER_DIALOG_RECOVERY: break;
    default: break;
    }
    solar_os_gfx_text(gfx, x + 5, y + 15, prompt);
    if (writer.dialog != WRITER_DIALOG_EXIT && writer.dialog != WRITER_DIALOG_RECOVERY) {
        char input[WRITER_DIALOG_INPUT_MAX + 2U];
        snprintf(input, sizeof(input), "%s_", writer.dialog_input);
        const size_t max_chars = w > 14 ? (size_t)((w - 14) / 7) : 1U;
        const char *visible = strlen(input) > max_chars ? &input[strlen(input) - max_chars] : input;
        solar_os_gfx_text(gfx, x + 5, y + 34, visible);
    }
}

static void writer_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || writer.suspended) {
        return;
    }
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    writer_draw_header(gfx);
    {
        solar_os_doc_view_t view = {
            .x = WRITER_MARGIN_X,
            .y = WRITER_HEADER_H + WRITER_MARGIN_Y,
            .width = (int)solar_os_gfx_width(gfx) - (2 * WRITER_MARGIN_X) - 4,
            .height = writer_view_height(gfx),
            .scroll_y = writer.scroll_y,
            .zoom = writer.zoom,
        };
        writer_draw_highlight(gfx, &view);
        solar_os_doc_layout_render(gfx, &writer.doc, &writer.layout, &view);
        int cursor_x = 0;
        int cursor_y = 0;
        int cursor_h = 14;
        if (writer.cursor_visible &&
            solar_os_doc_layout_source_to_xy(&writer.layout,
                                             writer.cursor,
                                             &cursor_x,
                                             &cursor_y,
                                             &cursor_h)) {
            const int x = view.x + cursor_x;
            const int y = view.y + cursor_y - view.scroll_y;
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_line(gfx, x, y, x, y + cursor_h - 1);
        }
        writer_draw_dialog(gfx);
    }
    solar_os_gfx_present(gfx);
    writer.render_pending = false;
}

static esp_err_t writer_start(solar_os_context_t *ctx)
{
    memset(&writer, 0, sizeof(writer));
    solar_os_writer_buffer_init(&writer.buffer);
    solar_os_doc_init(&writer.doc);
    solar_os_doc_layout_init(&writer.layout);
    writer.selection_anchor = SIZE_MAX;
    writer.saved_scroll_anchor = SIZE_MAX;
    writer.zoom = 1;
    writer.cursor_visible = true;

    if (solar_os_context_gfx(ctx) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_context_set_graphics_active(ctx, true);
    const int argc = solar_os_context_argc(ctx);
    if (argc > 2) {
        solar_os_context_finish(ctx, 2, "usage: writer [file.md]");
        return ESP_OK;
    }
    if (!solar_os_storage_is_mounted()) {
        solar_os_context_finish(ctx, 1, "writer: storage not mounted");
        return ESP_OK;
    }

    char *source = NULL;
    size_t source_len = 0;
    if (argc == 2) {
        const char *arg = solar_os_context_argv(ctx, 1);
        esp_err_t ret = solar_os_storage_resolve_path(arg, writer.path, sizeof(writer.path));
        if (ret != ESP_OK) {
            solar_os_context_finish(
                ctx,
                1,
                ret == ESP_ERR_INVALID_SIZE ?
                    "writer: path too long" : "writer: invalid path");
            return ESP_OK;
        }
        strlcpy(writer.display_name, arg, sizeof(writer.display_name));
        ret = writer_read_file(writer.path, &source, &source_len);
        if (ret == ESP_ERR_NOT_FOUND) {
            source = NULL;
            source_len = 0;
        } else if (ret != ESP_OK) {
            solar_os_context_finish(
                ctx,
                1,
                ret == ESP_ERR_INVALID_SIZE ?
                    "writer: file exceeds 256 KiB; use edit or reader" :
                    "writer: open failed");
            return ESP_OK;
        }
    } else {
        strlcpy(writer.display_name, "untitled.md", sizeof(writer.display_name));
    }

    esp_err_t ret = solar_os_writer_buffer_set(&writer.buffer, source, source_len);
    solar_os_memory_free(source);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = writer_prepare_state_paths();
    if (ret != ESP_OK) {
        return ret;
    }
    writer_load_metadata();
    writer.loaded = true;
    writer.parse_pending = true;
    writer.layout_pending = true;
    writer.render_pending = true;
    ret = writer_update_document(ctx);
    if (ret != ESP_OK) {
        return ret;
    }
    writer.recovery_available = writer_recovery_differs();
    if (writer.recovery_available) {
        writer.dialog = WRITER_DIALOG_RECOVERY;
    }
    writer_render(ctx);
    return ESP_OK;
}

static void writer_suspend(solar_os_context_t *ctx)
{
    (void)writer_write_recovery();
    writer_save_metadata();
    writer.suspended = true;
    solar_os_context_set_graphics_active(ctx, false);
}

static void writer_resume(solar_os_context_t *ctx)
{
    writer.suspended = false;
    writer_wake_cursor();
    solar_os_context_set_graphics_active(ctx, true);
    writer.layout_pending = true;
    writer.render_pending = true;
    if (writer_update_document(ctx) == ESP_OK) {
        writer_render(ctx);
    }
}

static void writer_stop(solar_os_context_t *ctx)
{
    if (writer.dirty && !writer.discard_on_stop) {
        (void)writer_write_recovery();
    }
    writer_save_metadata();
    solar_os_writer_buffer_free(&writer.buffer);
    solar_os_doc_free(&writer.doc);
    solar_os_doc_layout_free(&writer.layout);
    memset(&writer, 0, sizeof(writer));
    solar_os_context_set_graphics_active(ctx, false);
}

static bool writer_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        writer_resume(ctx);
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return writer_handle_char(ctx, (uint8_t)event->data.ch);
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        writer.last_tick_ms = event->data.tick_ms;
        if (writer.dirty && !writer.recovery_written &&
            event->data.tick_ms - writer.last_edit_ms >= WRITER_IDLE_RECOVERY_MS) {
            if (writer_write_recovery() != ESP_OK) {
                writer_set_message("recovery write failed");
            }
        }
        if (writer.parse_pending || writer.layout_pending) {
            esp_err_t ret = writer_update_document(ctx);
            if (ret != ESP_OK) {
                writer_set_message("layout failed");
            }
        }
        if (!writer.suspended &&
            writer.dialog == WRITER_DIALOG_NONE) {
            if (writer.cursor_blink_ms == 0) {
                writer.cursor_blink_ms = event->data.tick_ms;
            } else if (event->data.tick_ms - writer.cursor_blink_ms >=
                       WRITER_CURSOR_BLINK_MS) {
                writer.cursor_visible = !writer.cursor_visible;
                writer.cursor_blink_ms = event->data.tick_ms;
                writer.render_pending = true;
            }
        }
        if (writer.render_pending) {
            writer_render(ctx);
        }
        return true;
    }
    return false;
}

static void writer_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    snprintf(buffer,
             buffer_len,
             "writer %s",
             writer.display_name[0] != '\0' ? writer.display_name : "untitled.md");
}

const solar_os_app_t solar_os_writer_app = {
    .name = "writer",
    .summary = "hybrid WYSIWYG Markdown editor",
    .app_class = SOLAR_OS_APP_CLASS_GUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = writer_start,
    .suspend = writer_suspend,
    .resume = writer_resume,
    .stop = writer_stop,
    .event = writer_event,
    .title = writer_title,
    .state_slot = &writer_state,
    .state_size = sizeof(writer_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 40,
    .tick_deadline_ms = 40,
};
