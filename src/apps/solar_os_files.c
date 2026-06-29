#include "solar_os_files.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "solar_os_app_registry.h"
#include "solar_os_keys.h"
#include "solar_os_shell.h"
#include "solar_os_storage.h"
#include "solar_os_terminal.h"
#include "solar_os_tui.h"

#define FILES_NAME_MAX 96
#define FILES_MESSAGE_MAX 96
#define FILES_INPUT_MAX 80
#define FILES_INITIAL_CAPACITY 32U
#define FILES_PANEL_MIN_WIDTH 18U

typedef struct {
    char name[FILES_NAME_MAX];
    uint64_t size;
    bool is_dir;
    bool parent;
} files_entry_t;

typedef struct {
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    files_entry_t *entries;
    size_t count;
    size_t capacity;
    size_t cursor;
    size_t top;
    bool loaded;
    esp_err_t last_error;
} files_pane_t;

typedef enum {
    FILES_INPUT_NONE,
    FILES_INPUT_MKDIR,
    FILES_INPUT_DELETE_CONFIRM,
} files_input_mode_t;

typedef struct {
    solar_os_tui_t tui;
    files_pane_t panes[2];
    uint8_t active;
    bool show_hidden;
    files_input_mode_t input_mode;
    char input[FILES_INPUT_MAX];
    size_t input_len;
    char message[FILES_MESSAGE_MAX];
} files_state_t;

static files_state_t files;

static void *files_realloc(void *ptr, size_t size)
{
    void *next = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (next == NULL) {
        next = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    }
    return next;
}

static void files_free(void *ptr)
{
    heap_caps_free(ptr);
}

static files_pane_t *files_active_pane(void)
{
    return &files.panes[files.active & 1U];
}

static files_pane_t *files_other_pane(void)
{
    return &files.panes[(files.active ^ 1U) & 1U];
}

static const char *files_basename(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return "";
    }

    const char *end = path + strlen(path);
    while (end > path + 1 && end[-1] == '/') {
        end--;
    }
    const char *base = end;
    while (base > path && base[-1] != '/') {
        base--;
    }
    return base;
}

static bool files_is_hidden_name(const char *name)
{
    return name != NULL && name[0] == '.' && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static bool files_join_path(char *out, size_t out_len, const char *dir, const char *name)
{
    if (out == NULL || out_len == 0 || dir == NULL || name == NULL) {
        return false;
    }

    const size_t dir_len = strlen(dir);
    const int written = dir_len > 0 && dir[dir_len - 1] == '/' ?
        snprintf(out, out_len, "%s%s", dir, name) :
        snprintf(out, out_len, "%s/%s", dir, name);
    return written >= 0 && (size_t)written < out_len;
}

static bool files_parent_path(const char *path, char *out, size_t out_len)
{
    char mount[SOLAR_OS_STORAGE_MOUNT_POINT_MAX];
    if (path == NULL || out == NULL || out_len == 0) {
        return false;
    }
    if (solar_os_storage_path_mount_point(path, mount, sizeof(mount)) != ESP_OK) {
        strlcpy(mount, solar_os_storage_mount_point(), sizeof(mount));
    }

    size_t mount_len = strlen(mount);
    while (mount_len > 1 && mount[mount_len - 1] == '/') {
        mount_len--;
    }

    size_t len = strlen(path);
    while (len > mount_len && path[len - 1] == '/') {
        len--;
    }
    if (len <= mount_len) {
        strlcpy(out, mount, out_len);
        return true;
    }

    while (len > mount_len && path[len - 1] != '/') {
        len--;
    }
    if (len > mount_len && path[len - 1] == '/') {
        len--;
    }
    if (len < mount_len) {
        len = mount_len;
    }
    if (len + 1 > out_len) {
        return false;
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return true;
}

static void files_format_size(uint64_t size, char *out, size_t out_len)
{
    static const char units[] = {'B', 'K', 'M', 'G'};
    uint64_t scaled_x10 = size * 10U;
    size_t unit = 0;

    if (out == NULL || out_len == 0) {
        return;
    }

    while (scaled_x10 >= 10240U && unit + 1 < sizeof(units)) {
        scaled_x10 = (scaled_x10 + 512U) / 1024U;
        unit++;
    }

    if (unit == 0) {
        snprintf(out, out_len, "%lluB", (unsigned long long)(scaled_x10 / 10U));
    } else if (scaled_x10 < 100U) {
        snprintf(out,
                 out_len,
                 "%llu.%llu%c",
                 (unsigned long long)(scaled_x10 / 10U),
                 (unsigned long long)(scaled_x10 % 10U),
                 units[unit]);
    } else {
        snprintf(out,
                 out_len,
                 "%llu%c",
                 (unsigned long long)((scaled_x10 + 5U) / 10U),
                 units[unit]);
    }
}

static int files_name_cmp(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        const int ca = tolower((unsigned char)*a);
        const int cb = tolower((unsigned char)*b);
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int files_entry_cmp(const void *a, const void *b)
{
    const files_entry_t *ea = (const files_entry_t *)a;
    const files_entry_t *eb = (const files_entry_t *)b;

    if (ea->parent != eb->parent) {
        return ea->parent ? -1 : 1;
    }
    if (ea->is_dir != eb->is_dir) {
        return ea->is_dir ? -1 : 1;
    }
    return files_name_cmp(ea->name, eb->name);
}

static void files_pane_clear(files_pane_t *pane)
{
    if (pane == NULL) {
        return;
    }
    files_free(pane->entries);
    memset(pane, 0, sizeof(*pane));
}

static esp_err_t files_pane_reserve(files_pane_t *pane, size_t needed)
{
    if (needed <= pane->capacity) {
        return ESP_OK;
    }

    size_t next_capacity = pane->capacity > 0 ? pane->capacity : FILES_INITIAL_CAPACITY;
    while (next_capacity < needed) {
        next_capacity *= 2U;
    }

    files_entry_t *next = files_realloc(pane->entries, next_capacity * sizeof(next[0]));
    if (next == NULL) {
        return ESP_ERR_NO_MEM;
    }
    pane->entries = next;
    pane->capacity = next_capacity;
    return ESP_OK;
}

static esp_err_t files_pane_add(files_pane_t *pane,
                                const char *name,
                                bool is_dir,
                                bool parent,
                                uint64_t size)
{
    esp_err_t err = files_pane_reserve(pane, pane->count + 1U);
    if (err != ESP_OK) {
        return err;
    }

    files_entry_t *entry = &pane->entries[pane->count++];
    memset(entry, 0, sizeof(*entry));
    strlcpy(entry->name, name != NULL ? name : "", sizeof(entry->name));
    entry->is_dir = is_dir;
    entry->parent = parent;
    entry->size = size;
    return ESP_OK;
}

static bool files_path_is_root(const char *path)
{
    char parent[SOLAR_OS_STORAGE_PATH_MAX];
    if (!files_parent_path(path, parent, sizeof(parent))) {
        return true;
    }
    return strcmp(parent, path) == 0;
}

static size_t files_trimmed_path_len(const char *path)
{
    size_t len = path != NULL ? strlen(path) : 0;
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    return len;
}

static bool files_paths_equal(const char *a, const char *b)
{
    const size_t a_len = files_trimmed_path_len(a);
    const size_t b_len = files_trimmed_path_len(b);
    return a != NULL && b != NULL && a_len == b_len && strncmp(a, b, a_len) == 0;
}

static bool files_path_inside(const char *parent, const char *child)
{
    if (parent == NULL || child == NULL) {
        return false;
    }
    const size_t parent_len = files_trimmed_path_len(parent);
    return strncmp(parent, child, parent_len) == 0 &&
        (child[parent_len] == '/' || child[parent_len] == '\0');
}

static esp_err_t files_pane_load(files_pane_t *pane, const char *path)
{
    if (pane == NULL || path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char normalized[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_normalize_path(path, normalized, sizeof(normalized));
    if (err != ESP_OK) {
        return err;
    }

    DIR *dir = opendir(normalized);
    if (dir == NULL) {
        pane->loaded = false;
        pane->last_error = ESP_FAIL;
        return ESP_FAIL;
    }

    files_free(pane->entries);
    pane->entries = NULL;
    pane->count = 0;
    pane->capacity = 0;
    pane->cursor = 0;
    pane->top = 0;
    strlcpy(pane->path, normalized, sizeof(pane->path));

    if (!files_path_is_root(normalized)) {
        err = files_pane_add(pane, "..", true, true, 0);
        if (err != ESP_OK) {
            closedir(dir);
            return err;
        }
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!files.show_hidden && files_is_hidden_name(entry->d_name)) {
            continue;
        }

        char child[SOLAR_OS_STORAGE_PATH_MAX];
        if (!files_join_path(child, sizeof(child), normalized, entry->d_name)) {
            continue;
        }

        struct stat st;
        const bool stat_ok = stat(child, &st) == 0;
        const bool is_dir = stat_ok && S_ISDIR(st.st_mode);
        const uint64_t size = stat_ok && !is_dir ? (uint64_t)st.st_size : 0;
        err = files_pane_add(pane, entry->d_name, is_dir, false, size);
        if (err != ESP_OK) {
            closedir(dir);
            return err;
        }
    }
    closedir(dir);

    if (pane->count > 1U) {
        qsort(pane->entries, pane->count, sizeof(pane->entries[0]), files_entry_cmp);
    }
    pane->loaded = true;
    pane->last_error = ESP_OK;
    return ESP_OK;
}

static void files_set_message(const char *message)
{
    strlcpy(files.message, message != NULL ? message : "", sizeof(files.message));
}

static void files_set_error(const char *operation, const char *path)
{
    char message[FILES_MESSAGE_MAX];
    snprintf(message,
             sizeof(message),
             "%s: %s",
             operation != NULL ? operation : "error",
             strerror(errno));
    if (path != NULL && path[0] != '\0') {
        const size_t used = strlen(message);
        if (used + 2 < sizeof(message)) {
            snprintf(message + used, sizeof(message) - used, ": %s", files_basename(path));
        }
    }
    files_set_message(message);
}

static files_entry_t *files_selected_entry(files_pane_t *pane)
{
    if (pane == NULL || pane->count == 0 || pane->cursor >= pane->count) {
        return NULL;
    }
    return &pane->entries[pane->cursor];
}

static bool files_selected_path(files_pane_t *pane, char *out, size_t out_len)
{
    files_entry_t *entry = files_selected_entry(pane);
    if (entry == NULL) {
        return false;
    }
    if (entry->parent) {
        return files_parent_path(pane->path, out, out_len);
    }
    return files_join_path(out, out_len, pane->path, entry->name);
}

static void files_clip(char *out, size_t out_len, const char *text, size_t width)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    if (text == NULL) {
        text = "";
    }

    size_t copy = strlen(text);
    if (copy > width) {
        copy = width;
    }
    if (copy >= out_len) {
        copy = out_len - 1U;
    }
    memcpy(out, text, copy);
    out[copy] = '\0';
}

static void files_add_clipped(size_t row,
                              size_t col,
                              size_t width,
                              const char *text,
                              uint8_t attr)
{
    char buffer[SOLAR_OS_TERMINAL_MAX_COLS + 1];
    const size_t clipped_width = width < SOLAR_OS_TERMINAL_MAX_COLS ? width : SOLAR_OS_TERMINAL_MAX_COLS;

    if (width == 0) {
        return;
    }
    files_clip(buffer, sizeof(buffer), text, clipped_width);
    solar_os_tui_addstr(&files.tui, row, col, buffer, attr);
}

static void files_draw_entry(files_pane_t *pane,
                             size_t pane_index,
                             size_t row,
                             size_t col,
                             size_t width,
                             size_t index)
{
    const bool active = pane_index == files.active;
    const bool selected = active && index == pane->cursor;
    const uint8_t base_attr = selected ? SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL;
    files_entry_t *entry = index < pane->count ? &pane->entries[index] : NULL;

    solar_os_tui_fill(&files.tui, row, col, 1, width, ' ', base_attr);
    if (entry == NULL || width < 4) {
        return;
    }

    char name[FILES_NAME_MAX + 2];
    if (entry->parent) {
        strlcpy(name, "../", sizeof(name));
    } else if (entry->is_dir) {
        snprintf(name, sizeof(name), "%s/", entry->name);
    } else {
        strlcpy(name, entry->name, sizeof(name));
    }

    char size_text[12] = "";
    if (entry->is_dir) {
        strlcpy(size_text, entry->parent ? "" : "<DIR>", sizeof(size_text));
    } else {
        files_format_size(entry->size, size_text, sizeof(size_text));
    }

    const size_t size_width = width >= 18 ? 8U : 0U;
    const size_t name_width = size_width > 0 && width > size_width + 1U ?
        width - size_width - 1U : width;
    uint8_t name_attr = base_attr;
    if (entry->is_dir) {
        name_attr |= SOLAR_OS_TUI_ATTR_BOLD;
    }
    files_add_clipped(row, col, name_width, name, name_attr);
    if (size_width > 0) {
        files_add_clipped(row, col + width - size_width, size_width, size_text, base_attr);
    }
}

static void files_draw_pane(files_pane_t *pane,
                            size_t pane_index,
                            size_t row,
                            size_t col,
                            size_t height,
                            size_t width)
{
    if (height < 3 || width < FILES_PANEL_MIN_WIDTH) {
        return;
    }

    const bool active = pane_index == files.active;
    const uint8_t title_attr = active ? SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD
                                      : SOLAR_OS_TUI_ATTR_BOLD;
    solar_os_tui_box(&files.tui, row, col, height, width, SOLAR_OS_TUI_ATTR_NORMAL);
    solar_os_tui_fill(&files.tui, row, col + 1, 1, width - 2U, ' ', title_attr);
    files_add_clipped(row, col + 2, width - 4U, pane->path, title_attr);

    const size_t list_row = row + 1U;
    const size_t list_col = col + 1U;
    const size_t list_height = height - 2U;
    const size_t list_width = width - 2U;

    if (pane->cursor < pane->top) {
        pane->top = pane->cursor;
    } else if (pane->cursor >= pane->top + list_height && list_height > 0) {
        pane->top = pane->cursor - list_height + 1U;
    }

    for (size_t i = 0; i < list_height; i++) {
        const size_t index = pane->top + i;
        if (index < pane->count) {
            files_draw_entry(pane, pane_index, list_row + i, list_col, list_width, index);
        } else {
            solar_os_tui_fill(&files.tui,
                              list_row + i,
                              list_col,
                              1,
                              list_width,
                              ' ',
                              SOLAR_OS_TUI_ATTR_NORMAL);
        }
    }
}

static void files_draw_bottom(size_t rows, size_t cols)
{
    const size_t msg_row = rows >= 2 ? rows - 2U : 0;
    const size_t key_row = rows >= 1 ? rows - 1U : 0;

    solar_os_tui_fill(&files.tui, msg_row, 0, 1, cols, ' ', SOLAR_OS_TUI_ATTR_NORMAL);
    if (files.input_mode == FILES_INPUT_MKDIR) {
        char prompt[FILES_INPUT_MAX + 12];
        snprintf(prompt, sizeof(prompt), "mkdir: %s", files.input);
        files_add_clipped(msg_row, 0, cols, prompt, SOLAR_OS_TUI_ATTR_NORMAL);
        solar_os_tui_move(&files.tui, msg_row, strlen("mkdir: ") + files.input_len);
    } else if (files.input_mode == FILES_INPUT_DELETE_CONFIRM) {
        files_add_clipped(msg_row, 0, cols, files.message, SOLAR_OS_TUI_ATTR_BOLD);
    } else {
        files_add_clipped(msg_row, 0, cols, files.message, SOLAR_OS_TUI_ATTR_NORMAL);
    }

    solar_os_tui_fill(&files.tui, key_row, 0, 1, cols, ' ', SOLAR_OS_TUI_ATTR_INVERSE);
    files_add_clipped(key_row,
                      0,
                      cols,
                      "F3/v View F4/e Edit F5/c Copy F6/m Move F7/n Mkdir F8/d Del",
                      SOLAR_OS_TUI_ATTR_INVERSE);
}

static void files_render(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&files.tui, files.input_mode == FILES_INPUT_MKDIR);
    const size_t rows = solar_os_tui_rows(&files.tui);
    const size_t cols = solar_os_tui_cols(&files.tui);
    if (rows < 6 || cols < FILES_PANEL_MIN_WIDTH * 2U) {
        solar_os_tui_clear(&files.tui);
        solar_os_tui_addstr(&files.tui, 0, 0, "files: terminal too small", SOLAR_OS_TUI_ATTR_NORMAL);
        solar_os_tui_refresh(&files.tui);
        return;
    }

    solar_os_tui_clear(&files.tui);
    solar_os_tui_fill(&files.tui, 0, 0, 1, cols, ' ', SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);
    files_add_clipped(0,
                      0,
                      cols,
                      files.show_hidden ? "files  hidden:on  Tab switch  Enter open  q quit"
                                        : "files  hidden:off Tab switch  Enter open  q quit",
                      SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);

    const size_t pane_row = 1;
    const size_t pane_height = rows - 3U;
    const size_t left_width = cols / 2U;
    const size_t right_width = cols - left_width;
    files_draw_pane(&files.panes[0], 0, pane_row, 0, pane_height, left_width);
    files_draw_pane(&files.panes[1], 1, pane_row, left_width, pane_height, right_width);
    files_draw_bottom(rows, cols);
    solar_os_tui_refresh(&files.tui);
}

static esp_err_t files_refresh_pane(files_pane_t *pane)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    size_t old_cursor = pane->cursor;
    strlcpy(path, pane->path, sizeof(path));
    esp_err_t err = files_pane_load(pane, path);
    if (err == ESP_OK && pane->count > 0) {
        pane->cursor = old_cursor < pane->count ? old_cursor : pane->count - 1U;
    }
    return err;
}

static void files_refresh_all(void)
{
    (void)files_refresh_pane(&files.panes[0]);
    (void)files_refresh_pane(&files.panes[1]);
}

static void files_move_cursor(files_pane_t *pane, int delta)
{
    if (pane == NULL || pane->count == 0) {
        return;
    }
    if (delta < 0) {
        const size_t amount = (size_t)(-delta);
        pane->cursor = amount > pane->cursor ? 0 : pane->cursor - amount;
    } else {
        pane->cursor += (size_t)delta;
        if (pane->cursor >= pane->count) {
            pane->cursor = pane->count - 1U;
        }
    }
}

static void files_page(files_pane_t *pane, bool down)
{
    const size_t rows = solar_os_tui_rows(&files.tui);
    const size_t page = rows > 6 ? rows - 6U : 1U;
    files_move_cursor(pane, down ? (int)page : -(int)page);
}

static bool files_change_dir(files_pane_t *pane, const char *path)
{
    esp_err_t err = files_pane_load(pane, path);
    if (err != ESP_OK) {
        files_set_error("open", path);
        return false;
    }
    files_set_message("");
    return true;
}

static bool files_path_has_suffix(const char *path, const char *suffix)
{
    if (path == NULL || suffix == NULL) {
        return false;
    }
    const size_t path_len = strlen(path);
    const size_t suffix_len = strlen(suffix);
    if (suffix_len > path_len) {
        return false;
    }
    const char *tail = &path[path_len - suffix_len];
    for (size_t i = 0; i < suffix_len; i++) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)suffix[i])) {
            return false;
        }
    }
    return true;
}

static bool files_app_available(const char *app_name)
{
    const solar_os_app_registry_entry_t *entry = solar_os_app_registry_find(app_name);
    return entry != NULL && entry->app != NULL;
}

static const char *files_default_viewer(const char *path)
{
    if (files_app_available("view") &&
        (files_path_has_suffix(path, ".png") ||
         files_path_has_suffix(path, ".jpg") ||
         files_path_has_suffix(path, ".jpeg") ||
         files_path_has_suffix(path, ".gif") ||
         files_path_has_suffix(path, ".webp") ||
         files_path_has_suffix(path, ".bmp") ||
         files_path_has_suffix(path, ".pnm") ||
         files_path_has_suffix(path, ".pbm") ||
         files_path_has_suffix(path, ".pgm") ||
         files_path_has_suffix(path, ".ppm"))) {
        return "view";
    }

    if (files_app_available("aplay") &&
        (files_path_has_suffix(path, ".wav") ||
         files_path_has_suffix(path, ".mp3"))) {
        return "aplay";
    }

    if (files_app_available("sheet") &&
        files_path_has_suffix(path, ".csv")) {
        return "sheet";
    }

    if (files_app_available("python") &&
        (files_path_has_suffix(path, ".py") ||
         files_path_has_suffix(path, ".pyw") ||
         files_path_has_suffix(path, ".mpy"))) {
        return "python";
    }

    if (files_app_available("lua") &&
        files_path_has_suffix(path, ".lua")) {
        return "lua";
    }

    if (files_app_available("reader") &&
        (files_path_has_suffix(path, ".md") ||
         files_path_has_suffix(path, ".markdown") ||
         files_path_has_suffix(path, ".epub") ||
         files_path_has_suffix(path, ".txt") ||
         files_path_has_suffix(path, ".text"))) {
        return "reader";
    }

    return "less";
}

static bool files_launch_app(solar_os_context_t *ctx, const char *app_name, const char *path)
{
    const solar_os_app_registry_entry_t *entry = solar_os_app_registry_find(app_name);
    if (entry == NULL || entry->app == NULL) {
        files_set_message("app not available");
        return false;
    }

    char app_arg[SOLAR_OS_APP_ARG_LEN];
    char path_arg[SOLAR_OS_APP_ARG_LEN];
    char *argv[] = {app_arg, path_arg};
    strlcpy(app_arg, app_name, sizeof(app_arg));
    strlcpy(path_arg, path, sizeof(path_arg));
    const esp_err_t err = solar_os_context_request_launch_ex(ctx,
                                                             entry->app,
                                                             2,
                                                             argv,
                                                             SOLAR_OS_LAUNCH_CHILD_RETURN);
    if (err != ESP_OK) {
        files_set_message(esp_err_to_name(err));
        return false;
    }
    return true;
}

static void files_open_selected(solar_os_context_t *ctx)
{
    files_pane_t *pane = files_active_pane();
    files_entry_t *entry = files_selected_entry(pane);
    char path[SOLAR_OS_STORAGE_PATH_MAX];

    if (entry == NULL || !files_selected_path(pane, path, sizeof(path))) {
        return;
    }
    if (entry->is_dir) {
        files_change_dir(pane, path);
        return;
    }
    files_launch_app(ctx, files_default_viewer(path), path);
}

static bool files_copy_recursive(const char *source, const char *dest)
{
    struct stat st;
    if (stat(source, &st) != 0) {
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        return solar_os_storage_copy_file(source, dest) == ESP_OK;
    }

    if (solar_os_storage_mkdir(dest) != ESP_OK && errno != EEXIST) {
        return false;
    }

    DIR *dir = opendir(source);
    if (dir == NULL) {
        return false;
    }

    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_source[SOLAR_OS_STORAGE_PATH_MAX];
        char child_dest[SOLAR_OS_STORAGE_PATH_MAX];
        if (!files_join_path(child_source, sizeof(child_source), source, entry->d_name) ||
            !files_join_path(child_dest, sizeof(child_dest), dest, entry->d_name) ||
            !files_copy_recursive(child_source, child_dest)) {
            ok = false;
            break;
        }
    }
    closedir(dir);
    return ok;
}

static bool files_remove_recursive(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        return solar_os_storage_remove(path) == ESP_OK;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        return false;
    }

    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child[SOLAR_OS_STORAGE_PATH_MAX];
        if (!files_join_path(child, sizeof(child), path, entry->d_name) ||
            !files_remove_recursive(child)) {
            ok = false;
            break;
        }
    }
    closedir(dir);
    return ok && solar_os_storage_rmdir(path) == ESP_OK;
}

static void files_copy_selected(void)
{
    files_pane_t *source_pane = files_active_pane();
    files_pane_t *dest_pane = files_other_pane();
    files_entry_t *entry = files_selected_entry(source_pane);
    char source[SOLAR_OS_STORAGE_PATH_MAX];
    char dest[SOLAR_OS_STORAGE_PATH_MAX];

    if (entry == NULL || entry->parent) {
        files_set_message("copy: no file selected");
        return;
    }
    if (!files_selected_path(source_pane, source, sizeof(source)) ||
        !files_join_path(dest, sizeof(dest), dest_pane->path, entry->name)) {
        files_set_message("copy: path too long");
        return;
    }
    if (files_paths_equal(source, dest)) {
        files_set_message("copy: source and destination are the same");
        return;
    }
    if (entry->is_dir && files_path_inside(source, dest)) {
        files_set_message("copy: destination is inside source");
        return;
    }

    if (!files_copy_recursive(source, dest)) {
        files_set_error("copy", source);
        return;
    }
    files_set_message("copied");
    files_refresh_all();
}

static void files_move_selected(void)
{
    files_pane_t *source_pane = files_active_pane();
    files_pane_t *dest_pane = files_other_pane();
    files_entry_t *entry = files_selected_entry(source_pane);
    char source[SOLAR_OS_STORAGE_PATH_MAX];
    char dest[SOLAR_OS_STORAGE_PATH_MAX];

    if (entry == NULL || entry->parent) {
        files_set_message("move: no file selected");
        return;
    }
    if (!files_selected_path(source_pane, source, sizeof(source)) ||
        !files_join_path(dest, sizeof(dest), dest_pane->path, entry->name)) {
        files_set_message("move: path too long");
        return;
    }
    if (files_paths_equal(source, dest)) {
        files_set_message("move: source and destination are the same");
        return;
    }
    if (entry->is_dir && files_path_inside(source, dest)) {
        files_set_message("move: destination is inside source");
        return;
    }

    if (solar_os_storage_rename(source, dest) != ESP_OK) {
        files_set_error("move", source);
        return;
    }
    files_set_message("moved");
    files_refresh_all();
}

static void files_begin_mkdir(void)
{
    files.input_mode = FILES_INPUT_MKDIR;
    files.input_len = 0;
    files.input[0] = '\0';
    files_set_message("");
}

static void files_begin_delete(void)
{
    files_pane_t *pane = files_active_pane();
    files_entry_t *entry = files_selected_entry(pane);
    if (entry == NULL || entry->parent) {
        files_set_message("delete: no file selected");
        return;
    }

    files.input_mode = FILES_INPUT_DELETE_CONFIRM;
    snprintf(files.message, sizeof(files.message), "delete %s? y/N", entry->name);
}

static void files_delete_confirmed(void)
{
    files_pane_t *pane = files_active_pane();
    char path[SOLAR_OS_STORAGE_PATH_MAX];

    if (!files_selected_path(pane, path, sizeof(path))) {
        files_set_message("delete: no file selected");
        return;
    }
    if (!files_remove_recursive(path)) {
        files_set_error("delete", path);
    } else {
        files_set_message("deleted");
    }
    files.input_mode = FILES_INPUT_NONE;
    files_refresh_all();
}

static void files_create_directory(void)
{
    files_pane_t *pane = files_active_pane();
    char path[SOLAR_OS_STORAGE_PATH_MAX];

    if (files.input_len == 0) {
        files.input_mode = FILES_INPUT_NONE;
        files_set_message("");
        return;
    }
    if (!files_join_path(path, sizeof(path), pane->path, files.input)) {
        files_set_message("mkdir: path too long");
    } else if (solar_os_storage_mkdir(path) != ESP_OK) {
        files_set_error("mkdir", path);
    } else {
        files_set_message("created");
    }
    files.input_mode = FILES_INPUT_NONE;
    files_refresh_all();
}

static bool files_input_event(uint8_t ch)
{
    if (files.input_mode == FILES_INPUT_DELETE_CONFIRM) {
        if (ch == 'y' || ch == 'Y') {
            files_delete_confirmed();
        } else {
            files.input_mode = FILES_INPUT_NONE;
            files_set_message("");
        }
        return true;
    }

    if (files.input_mode != FILES_INPUT_MKDIR) {
        return false;
    }

    switch (ch) {
    case SOLAR_OS_KEY_ESCAPE:
        files.input_mode = FILES_INPUT_NONE;
        files_set_message("");
        return true;
    case '\r':
    case '\n':
        files_create_directory();
        return true;
    case '\b':
    case 0x7f:
        if (files.input_len > 0) {
            files.input[--files.input_len] = '\0';
        }
        return true;
    default:
        if ((isprint(ch) || ch >= 0xa0) && files.input_len + 1U < sizeof(files.input)) {
            files.input[files.input_len++] = (char)ch;
            files.input[files.input_len] = '\0';
        }
        return true;
    }
}

static esp_err_t files_start(solar_os_context_t *ctx)
{
    memset(&files, 0, sizeof(files));
    files.show_hidden = true;
    esp_err_t err = solar_os_tui_begin(&files.tui, ctx);
    if (err != ESP_OK) {
        return err;
    }
    (void)solar_os_tui_enable_diff(&files.tui, true);

    const char *arg = solar_os_context_argc(ctx) >= 2 ? solar_os_context_argv(ctx, 1) : ".";
    char start[SOLAR_OS_STORAGE_PATH_MAX];
    err = solar_os_context_shell_session(ctx) != NULL ?
        solar_os_shell_resolve_path(ctx, arg, start, sizeof(start)) :
        solar_os_storage_resolve_path(arg, start, sizeof(start));
    if (err != ESP_OK) {
        solar_os_tui_end(&files.tui);
        return err;
    }

    struct stat st;
    if (stat(start, &st) == 0 && !S_ISDIR(st.st_mode)) {
        char parent[SOLAR_OS_STORAGE_PATH_MAX];
        if (files_parent_path(start, parent, sizeof(parent))) {
            strlcpy(start, parent, sizeof(start));
        }
    }

    err = files_pane_load(&files.panes[0], start);
    if (err != ESP_OK) {
        solar_os_tui_end(&files.tui);
        return err;
    }
    err = files_pane_load(&files.panes[1], start);
    if (err != ESP_OK) {
        files_pane_clear(&files.panes[0]);
        solar_os_tui_end(&files.tui);
        return err;
    }

    files_set_message("");
    solar_os_tui_set_cursor_visible(&files.tui, false);
    files_render(ctx);
    return ESP_OK;
}

static void files_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&files.tui, true);
    solar_os_tui_refresh(&files.tui);
    solar_os_tui_end(&files.tui);
    files_pane_clear(&files.panes[0]);
    files_pane_clear(&files.panes[1]);
    memset(&files, 0, sizeof(files));
}

static void files_resume(solar_os_context_t *ctx)
{
    files_refresh_all();
    solar_os_tui_set_cursor_visible(&files.tui, false);
    files_render(ctx);
}

static bool files_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL || event->type != SOLAR_OS_EVENT_CHAR) {
        return true;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    files_pane_t *pane = files_active_pane();

    if (files_input_event(ch)) {
        files_render(ctx);
        return true;
    }

    switch (ch) {
    case SOLAR_OS_KEY_ESCAPE:
    case SOLAR_OS_KEY_APP_EXIT:
    case SOLAR_OS_KEY_F10:
    case 'q':
    case 'Q':
        solar_os_context_request_exit(ctx);
        return true;
    case '\t':
    case SOLAR_OS_KEY_LEFT:
    case SOLAR_OS_KEY_RIGHT:
        files.active ^= 1U;
        break;
    case SOLAR_OS_KEY_UP:
    case 'k':
        files_move_cursor(pane, -1);
        break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        files_move_cursor(pane, 1);
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        files_page(pane, false);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        files_page(pane, true);
        break;
    case SOLAR_OS_KEY_HOME:
        pane->cursor = 0;
        break;
    case SOLAR_OS_KEY_END:
        pane->cursor = pane->count > 0 ? pane->count - 1U : 0;
        break;
    case '\b':
    case 0x7f: {
        char parent[SOLAR_OS_STORAGE_PATH_MAX];
        if (files_parent_path(pane->path, parent, sizeof(parent))) {
            files_change_dir(pane, parent);
        }
        break;
    }
    case '\r':
    case '\n':
        files_open_selected(ctx);
        break;
    case SOLAR_OS_KEY_F3:
    case 'v':
    case 'V': {
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        files_entry_t *entry = files_selected_entry(pane);
        if (entry != NULL && !entry->is_dir && files_selected_path(pane, path, sizeof(path))) {
            files_launch_app(ctx, files_default_viewer(path), path);
        }
        break;
    }
    case SOLAR_OS_KEY_F4:
    case 'e':
    case 'E': {
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        files_entry_t *entry = files_selected_entry(pane);
        if (entry != NULL && !entry->is_dir && files_selected_path(pane, path, sizeof(path))) {
            files_launch_app(ctx, "edit", path);
        }
        break;
    }
    case SOLAR_OS_KEY_F5:
    case 'c':
    case 'C':
        files_copy_selected();
        break;
    case SOLAR_OS_KEY_F6:
    case 'm':
    case 'M':
        files_move_selected();
        break;
    case SOLAR_OS_KEY_F7:
    case 'n':
    case 'N':
        files_begin_mkdir();
        break;
    case SOLAR_OS_KEY_F8:
    case SOLAR_OS_KEY_DELETE:
    case 'd':
    case 'D':
        files_begin_delete();
        break;
    case 'h':
    case 'H':
        files.show_hidden = !files.show_hidden;
        files_refresh_all();
        break;
    case 'r':
    case 'R':
        files_refresh_all();
        files_set_message("refreshed");
        break;
    default:
        return true;
    }

    files_render(ctx);
    return true;
}

const solar_os_app_t solar_os_files_app = {
    .name = "files",
    .summary = "two-pane file manager",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = files_start,
    .resume = files_resume,
    .stop = files_stop,
    .event = files_event,
};
