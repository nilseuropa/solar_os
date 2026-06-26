#include "solar_os_reader.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "solar_os_doc.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_storage.h"

#define READER_HEADER_H 16
#define READER_MARGIN_X 8
#define READER_MARGIN_Y 5
#define READER_MESSAGE_MAX 96
#define READER_MAX_ZOOM 4
#define READER_STATE_DIR ".reader"
#define READER_POSITIONS_FILE "positions"
#define READER_POSITIONS_TMP_FILE "positions.tmp"
#define READER_POSITION_LINE_MAX (SOLAR_OS_STORAGE_PATH_MAX + 64)
#define READER_ASSET_MAX_BYTES (1024U * 1024U)

typedef struct {
    solar_os_doc_t doc;
    solar_os_doc_layout_t layout;
    bool loaded;
    bool error_only;
    bool layout_valid;
    int scroll_y;
    int zoom;
    int content_height;
    int layout_width;
    int layout_zoom;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char display_name[SOLAR_OS_STORAGE_PATH_MAX];
    char message[READER_MESSAGE_MAX];
} reader_state_t;

static reader_state_t reader;

static esp_err_t reader_state_path(char *path, size_t path_len, const char *leaf)
{
    if (path == NULL || path_len == 0 || !solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *mount = solar_os_storage_mount_point();
    const int written = leaf == NULL ?
        snprintf(path, path_len, "%s/%s", mount, READER_STATE_DIR) :
        snprintf(path, path_len, "%s/%s/%s", mount, READER_STATE_DIR, leaf);
    return written >= 0 && (size_t)written < path_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t reader_ensure_state_dir(void)
{
    char dir[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t ret = reader_state_path(dir, sizeof(dir), NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    struct stat st;
    if (stat(dir, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    if (mkdir(dir, 0777) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static bool reader_file_exists(const char *path)
{
    struct stat st;
    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool reader_target_is_url(const char *target)
{
    return target != NULL && strstr(target, "://") != NULL;
}

static void *reader_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static esp_err_t reader_normalize_asset_target(const char *target, char *out, size_t out_len)
{
    if (target == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t len = strcspn(target, "?#");
    if (len == 0 || len >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, target, len);
    out[len] = '\0';
    return ESP_OK;
}

static esp_err_t reader_resolve_asset_path(const char *document_path,
                                           const char *target,
                                           char *out,
                                           size_t out_len)
{
    if (document_path == NULL || target == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (reader_target_is_url(target)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char clean_target[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t ret = reader_normalize_asset_target(target, clean_target, sizeof(clean_target));
    if (ret != ESP_OK) {
        return ret;
    }

    if (clean_target[0] == '/') {
        return solar_os_storage_resolve_path(clean_target, out, out_len);
    }

    const char *slash = strrchr(document_path, '/');
    if (slash == NULL) {
        return solar_os_storage_resolve_path(clean_target, out, out_len);
    }

    char raw[SOLAR_OS_STORAGE_PATH_MAX];
    const size_t dir_len = (size_t)(slash - document_path);
    if (dir_len + 1U + strlen(clean_target) + 1U > sizeof(raw)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(raw, document_path, dir_len);
    raw[dir_len] = '/';
    strlcpy(&raw[dir_len + 1U], clean_target, sizeof(raw) - dir_len - 1U);
    return solar_os_storage_normalize_path(raw, out, out_len);
}

static esp_err_t reader_doc_asset_read(void *user,
                                       const char *document_path,
                                       const char *target,
                                       uint8_t **out_data,
                                       size_t *out_len)
{
    (void)user;

    if (out_data == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    *out_len = 0;

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t ret = reader_resolve_asset_path(document_path, target, path, sizeof(path));
    if (ret != ESP_OK) {
        return ret;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size <= 0 || (uint64_t)st.st_size > READER_ASSET_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    uint8_t *data = reader_malloc((size_t)st.st_size);
    if (data == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    const size_t read_len = fread(data, 1, (size_t)st.st_size, file);
    const bool failed = ferror(file) || read_len != (size_t)st.st_size;
    fclose(file);
    if (failed) {
        heap_caps_free(data);
        return ESP_FAIL;
    }

    *out_data = data;
    *out_len = read_len;
    return ESP_OK;
}

static void reader_doc_asset_release(void *user, uint8_t *data)
{
    (void)user;
    heap_caps_free(data);
}

static bool reader_path_has_suffix(const char *path, const char *suffix)
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

static bool reader_path_is_markdown(const char *path)
{
    return reader_path_has_suffix(path, ".md") ||
        reader_path_has_suffix(path, ".markdown");
}

static void reader_draw_text_clipped(solar_os_gfx_t *gfx,
                                      int x,
                                      int baseline_y,
                                      int width,
                                      const char *text,
                                      int char_w)
{
    if (gfx == NULL || text == NULL || width <= 0 || char_w <= 0) {
        return;
    }

    char clipped[128];
    size_t max_chars = (size_t)(width / char_w);
    if (max_chars >= sizeof(clipped)) {
        max_chars = sizeof(clipped) - 1U;
    }

    const size_t len = strlen(text);
    const size_t n = len < max_chars ? len : max_chars;
    if (n > 0) {
        memcpy(clipped, text, n);
    }
    clipped[n] = '\0';
    solar_os_gfx_text(gfx, x, baseline_y, clipped);
}

static int reader_content_area_height(solar_os_gfx_t *gfx)
{
    const int screen_h = gfx != NULL ? (int)solar_os_gfx_height(gfx) : 0;
    const int height = screen_h - READER_HEADER_H - (2 * READER_MARGIN_Y);
    return height > 8 ? height : 8;
}

static int reader_content_width(solar_os_gfx_t *gfx)
{
    const int screen_w = gfx != NULL ? (int)solar_os_gfx_width(gfx) : 0;
    const int width = screen_w - (2 * READER_MARGIN_X) - 4;
    return width > 8 ? width : 8;
}

static int reader_max_scroll(solar_os_gfx_t *gfx)
{
    if (gfx == NULL || !reader.loaded) {
        return 0;
    }

    const int view_h = reader_content_area_height(gfx);
    const int max_scroll = reader.content_height - view_h;
    return max_scroll > 0 ? max_scroll : 0;
}

static void reader_clamp_scroll(solar_os_gfx_t *gfx)
{
    const int max_scroll = reader_max_scroll(gfx);
    if (reader.scroll_y < 0) {
        reader.scroll_y = 0;
    } else if (reader.scroll_y > max_scroll) {
        reader.scroll_y = max_scroll;
    }
}

static void reader_update_measure(solar_os_gfx_t *gfx)
{
    if (gfx == NULL || !reader.loaded) {
        reader.content_height = 0;
        return;
    }

    const int width = reader_content_width(gfx);
    if (!reader.layout_valid ||
        reader.layout_width != width ||
        reader.layout_zoom != reader.zoom) {
        const esp_err_t err = solar_os_doc_layout_build(&reader.layout,
                                                        &reader.doc,
                                                        width,
                                                        reader.zoom);
        if (err != ESP_OK) {
            reader.layout_valid = false;
            reader.content_height = 0;
            snprintf(reader.message,
                     sizeof(reader.message),
                     "layout failed: %s",
                     esp_err_to_name(err));
            return;
        }
        reader.layout_valid = true;
        reader.layout_width = width;
        reader.layout_zoom = reader.zoom;
    }
    reader.content_height = reader.layout.height;
    reader_clamp_scroll(gfx);
}

static bool reader_parse_position_line(char *line,
                                        uint64_t *offset,
                                        uint64_t *len,
                                        int *zoom,
                                        char **path)
{
    int path_index = 0;

    if (line == NULL || offset == NULL || len == NULL || zoom == NULL || path == NULL) {
        return false;
    }
    if (sscanf(line,
               "%" SCNu64 " %" SCNu64 " z=%d %n",
               offset,
               len,
               zoom,
               &path_index) != 3 ||
        path_index <= 0 ||
        line[path_index] == '\0') {
        return false;
    }

    *path = &line[path_index];
    while (isspace((unsigned char)**path)) {
        (*path)++;
    }
    (*path)[strcspn(*path, "\r\n")] = '\0';
    return (*path)[0] != '\0';
}

static bool reader_same_position_path(const char *line)
{
    char copy[READER_POSITION_LINE_MAX];
    uint64_t offset = 0;
    uint64_t len = 0;
    int zoom = 0;
    char *path = NULL;

    if (line == NULL) {
        return false;
    }

    strlcpy(copy, line, sizeof(copy));
    return reader_parse_position_line(copy, &offset, &len, &zoom, &path) &&
        strcmp(path, reader.path) == 0;
}

static size_t reader_anchor_offset_for_scroll(void)
{
    if (!reader.layout_valid || reader.layout.line_count == 0) {
        return 0;
    }

    const solar_os_doc_layout_line_t *best = &reader.layout.lines[0];
    for (size_t i = 0; i < reader.layout.line_count; i++) {
        const solar_os_doc_layout_line_t *line = &reader.layout.lines[i];
        if (line->y + line->height > reader.scroll_y) {
            best = line;
            break;
        }
        best = line;
    }

    if (best->source_start != SIZE_MAX) {
        return best->source_start <= reader.doc.source_len ? best->source_start : reader.doc.source_len;
    }
    return 0;
}

static void reader_scroll_to_anchor(solar_os_context_t *ctx, uint64_t saved_offset)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    reader_update_measure(gfx);
    if (!reader.layout_valid) {
        return;
    }

    size_t offset = saved_offset > reader.doc.source_len ?
        reader.doc.source_len :
        (size_t)saved_offset;
    int y = 0;
    int height = 0;
    if (solar_os_doc_layout_source_to_xy(&reader.layout, offset, NULL, &y, &height)) {
        reader.scroll_y = y;
        reader_clamp_scroll(gfx);
    }
}

static void reader_load_position(solar_os_context_t *ctx)
{
    char positions_path[SOLAR_OS_STORAGE_PATH_MAX];
    char line[READER_POSITION_LINE_MAX];

    if (reader_state_path(positions_path, sizeof(positions_path), READER_POSITIONS_FILE) != ESP_OK) {
        return;
    }

    FILE *file = fopen(positions_path, "r");
    if (file == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        uint64_t saved_offset = 0;
        uint64_t saved_len = 0;
        int saved_zoom = 0;
        char *saved_path = NULL;
        if (!reader_parse_position_line(line, &saved_offset, &saved_len, &saved_zoom, &saved_path) ||
            strcmp(saved_path, reader.path) != 0) {
            continue;
        }

        if (saved_zoom >= 0 && saved_zoom <= READER_MAX_ZOOM) {
            reader.zoom = saved_zoom;
        }
        reader.layout_valid = false;
        reader_scroll_to_anchor(ctx, saved_offset);
        snprintf(reader.message,
                 sizeof(reader.message),
                 saved_len == reader.doc.source_len ? "resumed" : "resumed, file changed");
        break;
    }

    fclose(file);
}

static void reader_save_position(solar_os_context_t *ctx)
{
    char positions_path[SOLAR_OS_STORAGE_PATH_MAX];
    char tmp_path[SOLAR_OS_STORAGE_PATH_MAX];
    char line[READER_POSITION_LINE_MAX];

    if (ctx == NULL ||
        !reader.loaded ||
        reader.error_only ||
        reader.path[0] == '\0' ||
        reader_ensure_state_dir() != ESP_OK ||
        reader_state_path(positions_path, sizeof(positions_path), READER_POSITIONS_FILE) != ESP_OK ||
        reader_state_path(tmp_path, sizeof(tmp_path), READER_POSITIONS_TMP_FILE) != ESP_OK) {
        return;
    }

    reader_update_measure(solar_os_context_gfx(ctx));
    const size_t offset = reader_anchor_offset_for_scroll();

    FILE *source = fopen(positions_path, "r");
    FILE *dest = fopen(tmp_path, "w");
    if (dest == NULL) {
        if (source != NULL) {
            fclose(source);
        }
        return;
    }

    if (source != NULL) {
        while (fgets(line, sizeof(line), source) != NULL) {
            if (!reader_same_position_path(line)) {
                fputs(line, dest);
            }
        }
        fclose(source);
    }

    fprintf(dest,
            "%" PRIu64 " %" PRIu64 " z=%d %s\n",
            (uint64_t)offset,
            (uint64_t)reader.doc.source_len,
            reader.zoom,
            reader.path);

    const bool ok = ferror(dest) == 0;
    fclose(dest);
    if (!ok) {
        (void)remove(tmp_path);
        return;
    }

    (void)remove(positions_path);
    if (rename(tmp_path, positions_path) != 0) {
        (void)remove(tmp_path);
    }
}

static void reader_draw_header(solar_os_gfx_t *gfx)
{
    const int screen_w = (int)solar_os_gfx_width(gfx);
    char title[192];

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, READER_HEADER_H);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

    if (reader.message[0] != '\0') {
        snprintf(title, sizeof(title), "reader z%d %s", reader.zoom, reader.message);
    } else {
        snprintf(title, sizeof(title), "reader z%d %s", reader.zoom, reader.display_name);
    }
    reader_draw_text_clipped(gfx, 3, 11, screen_w - 6, title, 6);
}

static void reader_draw_scrollbar(solar_os_gfx_t *gfx, const solar_os_doc_view_t *view)
{
    const int max_scroll = reader_max_scroll(gfx);
    if (gfx == NULL || view == NULL || max_scroll <= 0 || view->height <= 4) {
        return;
    }

    const int x = view->x + view->width + 2;
    if (x >= (int)solar_os_gfx_width(gfx)) {
        return;
    }

    int thumb_h = (view->height * view->height) / reader.content_height;
    if (thumb_h < 6) {
        thumb_h = 6;
    }
    if (thumb_h > view->height) {
        thumb_h = view->height;
    }
    const int track = view->height - thumb_h;
    const int thumb_y = view->y + ((track * reader.scroll_y) / max_scroll);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
    solar_os_gfx_line(gfx, x, view->y, x, view->y + view->height - 1);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, x - 1, thumb_y, 3, thumb_h);
}

static bool reader_line_for_scroll(size_t *line_index)
{
    if (line_index == NULL || !reader.layout_valid || reader.layout.line_count == 0) {
        return false;
    }

    size_t best = 0;
    for (size_t i = 0; i < reader.layout.line_count; i++) {
        const solar_os_doc_layout_line_t *line = &reader.layout.lines[i];
        if (line->y + line->height > reader.scroll_y) {
            *line_index = i;
            return true;
        }
        best = i;
    }

    *line_index = best;
    return true;
}

static void reader_scroll_to_line(solar_os_context_t *ctx, size_t line_index)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    reader_update_measure(gfx);
    if (!reader.layout_valid || reader.layout.line_count == 0) {
        return;
    }
    if (line_index >= reader.layout.line_count) {
        line_index = reader.layout.line_count - 1U;
    }
    reader.scroll_y = reader.layout.lines[line_index].y;
    reader_clamp_scroll(gfx);
}

static void reader_scroll_lines(solar_os_context_t *ctx, int delta_lines)
{
    reader_update_measure(solar_os_context_gfx(ctx));
    if (!reader.layout_valid || reader.layout.line_count == 0) {
        return;
    }

    size_t line_index = 0;
    if (!reader_line_for_scroll(&line_index)) {
        return;
    }

    int target = (int)line_index + delta_lines;
    if (target < 0) {
        target = 0;
    } else if (target >= (int)reader.layout.line_count) {
        target = (int)reader.layout.line_count - 1;
    }
    reader_scroll_to_line(ctx, (size_t)target);
}

static size_t reader_line_for_target_y(int target_y, bool forward)
{
    if (!reader.layout_valid || reader.layout.line_count == 0) {
        return 0;
    }

    size_t result = 0;
    if (forward) {
        for (size_t i = 0; i < reader.layout.line_count; i++) {
            if (reader.layout.lines[i].y >= target_y) {
                return i;
            }
            result = i;
        }
        return result;
    }

    for (size_t i = 0; i < reader.layout.line_count; i++) {
        if (reader.layout.lines[i].y > target_y) {
            break;
        }
        result = i;
    }
    return result;
}

static void reader_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    solar_os_doc_view_t view = {
        .x = READER_MARGIN_X,
        .y = READER_HEADER_H + READER_MARGIN_Y,
        .width = screen_w - (2 * READER_MARGIN_X) - 4,
        .height = screen_h - READER_HEADER_H - (2 * READER_MARGIN_Y),
        .scroll_y = reader.scroll_y,
        .zoom = reader.zoom,
    };
    if (view.width < 8) {
        view.width = 8;
    }
    if (view.height < 8) {
        view.height = 8;
    }

    if (reader.error_only) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_14);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        reader_draw_text_clipped(gfx,
                                  READER_MARGIN_X,
                                  READER_HEADER_H + 28,
                                  screen_w - (2 * READER_MARGIN_X),
                                  reader.message,
                                  7);
    } else {
        reader_update_measure(gfx);
        view.scroll_y = reader.scroll_y;
        if (reader.layout_valid) {
            solar_os_doc_layout_render(gfx, &reader.doc, &reader.layout, &view);
        }
        reader_draw_scrollbar(gfx, &view);
    }

    reader_draw_header(gfx);
    solar_os_gfx_present(gfx);
}

static esp_err_t reader_load_doc_file(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!reader_file_exists(path)) {
        return ESP_ERR_NOT_FOUND;
    }
    return solar_os_doc_load_path_as(&reader.doc, path, reader_path_is_markdown(path));
}

static esp_err_t reader_start(solar_os_context_t *ctx)
{
    memset(&reader, 0, sizeof(reader));
    solar_os_doc_init(&reader.doc);
    const solar_os_doc_asset_provider_t assets = {
        .read = reader_doc_asset_read,
        .release = reader_doc_asset_release,
        .user = NULL,
    };
    solar_os_doc_set_asset_provider(&reader.doc, &assets);
    solar_os_doc_layout_init(&reader.layout);
    reader.zoom = 1;

    if (solar_os_context_gfx(ctx) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const int argc = solar_os_context_argc(ctx);
    if (argc != 2) {
        reader.error_only = true;
        snprintf(reader.message, sizeof(reader.message), "usage: reader <file.md|file.txt>");
        solar_os_context_set_graphics_active(ctx, true);
        reader_render(ctx);
        return ESP_OK;
    }

    const char *arg = solar_os_context_argv(ctx, 1);
    strlcpy(reader.display_name, arg != NULL ? arg : "", sizeof(reader.display_name));

    esp_err_t err = solar_os_storage_resolve_path(arg, reader.path, sizeof(reader.path));
    if (err != ESP_OK) {
        reader.error_only = true;
        snprintf(reader.message, sizeof(reader.message), "invalid path: %s", esp_err_to_name(err));
        solar_os_context_set_graphics_active(ctx, true);
        reader_render(ctx);
        return ESP_OK;
    }

    if (reader.display_name[0] == '\0') {
        strlcpy(reader.display_name, reader.path, sizeof(reader.display_name));
    }

    err = reader_load_doc_file(reader.path);
    if (err != ESP_OK) {
        reader.error_only = true;
        snprintf(reader.message, sizeof(reader.message), "load failed: %s", esp_err_to_name(err));
        solar_os_context_set_graphics_active(ctx, true);
        reader_render(ctx);
        return ESP_OK;
    }

    reader.loaded = true;
    solar_os_context_set_graphics_active(ctx, true);
    reader_load_position(ctx);
    reader_render(ctx);
    return ESP_OK;
}

static void reader_stop(solar_os_context_t *ctx)
{
    reader_save_position(ctx);
    solar_os_doc_free(&reader.doc);
    solar_os_doc_layout_free(&reader.layout);
    memset(&reader, 0, sizeof(reader));
    solar_os_context_set_graphics_active(ctx, false);
}

static void reader_page(solar_os_context_t *ctx, bool down)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    reader_update_measure(gfx);
    if (!reader.layout_valid || reader.layout.line_count == 0) {
        return;
    }

    size_t line_index = 0;
    if (!reader_line_for_scroll(&line_index)) {
        return;
    }

    const solar_os_doc_layout_line_t *line = &reader.layout.lines[line_index];
    const int view_h = reader_content_area_height(gfx);
    int overlap = line->height;
    if (overlap < 1) {
        overlap = 1;
    }
    int step = view_h - overlap;
    if (step < overlap) {
        step = view_h > 1 ? view_h - 1 : 1;
    }

    const int target_y = down ? reader.scroll_y + step : reader.scroll_y - step;
    const size_t target_line = reader_line_for_target_y(target_y, down);
    reader_scroll_to_line(ctx, target_line);
}

static void reader_zoom(solar_os_context_t *ctx, int delta)
{
    reader_update_measure(solar_os_context_gfx(ctx));
    const size_t anchor = reader_anchor_offset_for_scroll();
    const int old_zoom = reader.zoom;
    reader.zoom += delta;
    if (reader.zoom < 0) {
        reader.zoom = 0;
    } else if (reader.zoom > READER_MAX_ZOOM) {
        reader.zoom = READER_MAX_ZOOM;
    }
    if (reader.zoom != old_zoom) {
        reader.layout_valid = false;
        reader_update_measure(solar_os_context_gfx(ctx));
        reader_scroll_to_anchor(ctx, anchor);
    }
}

static bool reader_handle_char(solar_os_context_t *ctx, char raw_ch)
{
    const uint8_t ch = (uint8_t)raw_ch;

    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (reader.error_only) {
        return true;
    }

    switch (ch) {
    case SOLAR_OS_KEY_UP:
        reader_scroll_lines(ctx, -1);
        break;
    case SOLAR_OS_KEY_DOWN:
        reader_scroll_lines(ctx, 1);
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        reader_page(ctx, false);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        reader_page(ctx, true);
        break;
    case SOLAR_OS_KEY_HOME:
        reader.scroll_y = 0;
        break;
    case SOLAR_OS_KEY_END:
        reader.scroll_y = reader_max_scroll(solar_os_context_gfx(ctx));
        break;
    case SOLAR_OS_KEY_CTRL_PLUS:
    case '+':
    case '=':
        reader_zoom(ctx, 1);
        break;
    case 0x1f:
    case '-':
        reader_zoom(ctx, -1);
        break;
    default:
        break;
    }
    return true;
}

static bool reader_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        reader_render(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return true;
    }

    const bool handled = reader_handle_char(ctx, event->data.ch);
    if (handled) {
        reader_render(ctx);
    }
    return handled;
}

const solar_os_app_t solar_os_reader_app = {
    .name = "reader",
    .summary = "graphics Markdown/text reader",
    .start = reader_start,
    .stop = reader_stop,
    .event = reader_event,
};
