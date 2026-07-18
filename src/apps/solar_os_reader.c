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
#include "solar_os_doc.h"
#include "solar_os_epub.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

#define READER_HEADER_H 16
#define READER_MARGIN_X 8
#define READER_MARGIN_Y 5
#define READER_MESSAGE_MAX 96
#define READER_SEARCH_MAX 64
#define READER_MAX_ZOOM 4
#define READER_STATE_DIR ".reader"
#define READER_POSITIONS_FILE "positions"
#define READER_POSITIONS_TMP_FILE "positions.tmp"
#define READER_POSITION_LINE_MAX (SOLAR_OS_STORAGE_PATH_MAX + 64)
#define READER_ASSET_MAX_BYTES (1024U * 1024U)

typedef struct {
    bool valid;
    size_t block_index;
    size_t text_offset;
    size_t text_len;
} reader_search_match_t;

typedef struct {
    solar_os_doc_t doc;
    solar_os_doc_layout_t layout;
    solar_os_epub_book_t *epub_book;
    bool loaded;
    bool error_only;
    bool suspended;
    bool epub;
    bool layout_valid;
    int scroll_y;
    int zoom;
    int content_height;
    int layout_width;
    int layout_zoom;
    size_t epub_chapter;
    size_t epub_chapter_count;
    bool search_input;
    bool search_status;
    char search[READER_SEARCH_MAX];
    size_t search_len;
    char last_search[READER_SEARCH_MAX];
    size_t last_search_len;
    reader_search_match_t match;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char display_name[SOLAR_OS_STORAGE_PATH_MAX];
    char message[READER_MESSAGE_MAX];
} reader_state_t;

typedef struct {
    bool found;
    uint64_t offset;
    uint64_t len;
    int zoom;
    size_t chapter;
} reader_saved_position_t;

static reader_state_t reader;

static esp_err_t reader_state_path(char *path, size_t path_len, const char *leaf)
{
    if (path == NULL || path_len == 0 || !solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (leaf == NULL || leaf[0] == '\0') {
        return solar_os_storage_default_path(READER_STATE_DIR, path, path_len);
    }

    char dir[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t ret = solar_os_storage_default_path(READER_STATE_DIR, dir, sizeof(dir));
    if (ret != ESP_OK) {
        return ret;
    }
    return solar_os_storage_join_path(dir, leaf, path, path_len);
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

static bool reader_document_is_epub(const char *document_path)
{
    if (document_path == NULL) {
        return false;
    }
    const char *pipe = strchr(document_path, '|');
    if (pipe != NULL) {
        char archive[SOLAR_OS_STORAGE_PATH_MAX];
        const size_t len = (size_t)(pipe - document_path);
        if (len == 0 || len >= sizeof(archive)) {
            return false;
        }
        memcpy(archive, document_path, len);
        archive[len] = '\0';
        return reader_path_has_suffix(archive, ".epub");
    }
    return reader_path_has_suffix(document_path, ".epub");
}

static void *reader_malloc(size_t size)
{
    return solar_os_memory_alloc(size,
                                 SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                 "reader.asset");
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

    if (reader_document_is_epub(document_path)) {
        return solar_os_epub_asset_read(user, document_path, target, out_data, out_len);
    }

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
        solar_os_memory_free(data);
        return ESP_FAIL;
    }

    *out_data = data;
    *out_len = read_len;
    return ESP_OK;
}

static void reader_doc_asset_release(void *user, uint8_t *data)
{
    (void)user;
    solar_os_memory_free(data);
}

static bool reader_path_is_markdown(const char *path)
{
    return reader_path_has_suffix(path, ".md") ||
        reader_path_has_suffix(path, ".markdown");
}

static bool reader_path_is_epub(const char *path)
{
    return reader_path_has_suffix(path, ".epub");
}

static esp_err_t reader_load_epub_chapter(size_t chapter)
{
    if (reader.epub_book == NULL || chapter >= reader.epub_chapter_count) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = solar_os_epub_load_spine_doc(reader.epub_book, chapter, &reader.doc);
    if (ret != ESP_OK) {
        return ret;
    }

    reader.epub_chapter = chapter;
    reader.scroll_y = 0;
    reader.layout_valid = false;
    reader.content_height = 0;
    reader.match.valid = false;
    reader.search_status = false;
    reader.message[0] = '\0';
    return ESP_OK;
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

static bool reader_is_printable(uint8_t ch)
{
    return ch >= 32U && ch < 127U;
}

static bool reader_text_match_at(const char *text,
                                 size_t text_len,
                                 size_t offset,
                                 const char *query,
                                 size_t query_len)
{
    if (text == NULL ||
        query == NULL ||
        query_len == 0 ||
        offset > text_len ||
        query_len > text_len - offset) {
        return false;
    }

    for (size_t i = 0; i < query_len; i++) {
        if (tolower((unsigned char)text[offset + i]) !=
            tolower((unsigned char)query[i])) {
            return false;
        }
    }
    return true;
}

static bool reader_match_after(size_t block_index,
                               size_t text_offset,
                               size_t start_block,
                               size_t start_offset)
{
    return block_index > start_block ||
        (block_index == start_block && text_offset >= start_offset);
}

static bool reader_match_before(size_t block_index,
                                size_t text_offset,
                                size_t start_block,
                                size_t start_offset)
{
    return block_index < start_block ||
        (block_index == start_block && text_offset < start_offset);
}

static bool reader_current_search_position(size_t *block_index, size_t *text_offset)
{
    if (block_index == NULL || text_offset == NULL) {
        return false;
    }
    *block_index = 0;
    *text_offset = 0;

    if (!reader.layout_valid || reader.layout.line_count == 0) {
        return reader.doc.block_count > 0;
    }

    const solar_os_doc_layout_line_t *line = &reader.layout.lines[0];
    for (size_t i = 0; i < reader.layout.line_count; i++) {
        line = &reader.layout.lines[i];
        if (line->y + line->height > reader.scroll_y) {
            break;
        }
    }

    *block_index = line->block_index;
    *text_offset = 0;
    for (size_t i = 0; i < line->run_count; i++) {
        const solar_os_doc_layout_run_t *run = &reader.layout.runs[line->run_start + i];
        if (run->block_index == line->block_index && run->text_offset != SIZE_MAX) {
            *text_offset = run->text_offset;
            return true;
        }
    }
    return true;
}

static bool reader_find_text(const char *query,
                             size_t query_len,
                             size_t start_block,
                             size_t start_offset,
                             bool forward,
                             bool *wrapped)
{
    bool have_first = false;
    bool have_last = false;
    bool have_candidate = false;
    reader_search_match_t first = {0};
    reader_search_match_t last = {0};
    reader_search_match_t candidate = {0};

    if (wrapped != NULL) {
        *wrapped = false;
    }
    if (query == NULL || query_len == 0 || reader.doc.block_count == 0) {
        reader.match.valid = false;
        return false;
    }
    if (start_block >= reader.doc.block_count) {
        start_block = reader.doc.block_count - 1U;
    }

    for (size_t b = 0; b < reader.doc.block_count; b++) {
        const solar_os_doc_block_t *block = &reader.doc.blocks[b];
        const char *text = block->text != NULL ? block->text : "";
        const size_t text_len = strlen(text);
        if (query_len > text_len) {
            continue;
        }

        for (size_t pos = 0; pos <= text_len - query_len; pos++) {
            if (!reader_text_match_at(text, text_len, pos, query, query_len)) {
                continue;
            }

            reader_search_match_t found = {
                .valid = true,
                .block_index = b,
                .text_offset = pos,
                .text_len = query_len,
            };

            if (!have_first) {
                first = found;
                have_first = true;
            }
            last = found;
            have_last = true;

            if (forward) {
                if (!have_candidate &&
                    reader_match_after(b, pos, start_block, start_offset)) {
                    candidate = found;
                    have_candidate = true;
                }
            } else if (reader_match_before(b, pos, start_block, start_offset)) {
                candidate = found;
                have_candidate = true;
            }
        }
    }

    if (have_candidate) {
        reader.match = candidate;
        return true;
    }

    if (forward && have_first) {
        reader.match = first;
        if (wrapped != NULL) {
            *wrapped = true;
        }
        return true;
    }
    if (!forward && have_last) {
        reader.match = last;
        if (wrapped != NULL) {
            *wrapped = true;
        }
        return true;
    }

    reader.match.valid = false;
    return false;
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
                                        size_t *chapter,
                                        char **path)
{
    int path_index = 0;
    int chapter_index = 0;
    unsigned parsed_chapter = 0;

    if (line == NULL ||
        offset == NULL ||
        len == NULL ||
        zoom == NULL ||
        chapter == NULL ||
        path == NULL) {
        return false;
    }
    *chapter = 0;
    if (sscanf(line,
               "%" SCNu64 " %" SCNu64 " z=%d c=%u %n",
               offset,
               len,
               zoom,
               &parsed_chapter,
               &path_index) == 4 &&
        path_index > 0 &&
        line[path_index] != '\0') {
        *chapter = parsed_chapter;
    } else if (sscanf(line,
                      "%" SCNu64 " %" SCNu64 " z=%d %n",
                      offset,
                      len,
                      zoom,
                      &chapter_index) == 3 &&
               chapter_index > 0 &&
               line[chapter_index] != '\0') {
        path_index = chapter_index;
        *chapter = 0;
    } else {
        return false;
    }

    if (path_index <= 0 || line[path_index] == '\0') {
        return false;
    }

    *path = &line[path_index];
    while (isspace((unsigned char)**path)) {
        (*path)++;
    }
    (*path)[strcspn(*path, "\r\n")] = '\0';
    return (*path)[0] != '\0';
}

static bool reader_read_saved_position(reader_saved_position_t *saved)
{
    char positions_path[SOLAR_OS_STORAGE_PATH_MAX];
    char line[READER_POSITION_LINE_MAX];

    if (saved == NULL) {
        return false;
    }
    memset(saved, 0, sizeof(*saved));

    if (reader_state_path(positions_path, sizeof(positions_path), READER_POSITIONS_FILE) != ESP_OK) {
        return false;
    }

    FILE *file = fopen(positions_path, "r");
    if (file == NULL) {
        return false;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *saved_path = NULL;
        size_t chapter = 0;
        if (!reader_parse_position_line(line,
                                        &saved->offset,
                                        &saved->len,
                                        &saved->zoom,
                                        &chapter,
                                        &saved_path) ||
            strcmp(saved_path, reader.path) != 0) {
            continue;
        }
        saved->chapter = chapter;
        saved->found = true;
        break;
    }

    fclose(file);
    return saved->found;
}

static bool reader_same_position_path(const char *line)
{
    char copy[READER_POSITION_LINE_MAX];
    uint64_t offset = 0;
    uint64_t len = 0;
    int zoom = 0;
    size_t chapter = 0;
    char *path = NULL;

    if (line == NULL) {
        return false;
    }

    strlcpy(copy, line, sizeof(copy));
    return reader_parse_position_line(copy, &offset, &len, &zoom, &chapter, &path) &&
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

static bool reader_switch_epub_chapter(solar_os_context_t *ctx, size_t chapter, bool end)
{
    if (!reader.epub || chapter >= reader.epub_chapter_count) {
        return false;
    }

    const esp_err_t err = reader_load_epub_chapter(chapter);
    if (err != ESP_OK) {
        snprintf(reader.message, sizeof(reader.message), "chapter load failed: %s", esp_err_to_name(err));
        return true;
    }

    reader_update_measure(solar_os_context_gfx(ctx));
    reader.scroll_y = end ? reader_max_scroll(solar_os_context_gfx(ctx)) : 0;
    reader_clamp_scroll(solar_os_context_gfx(ctx));
    return true;
}

static void reader_load_position(solar_os_context_t *ctx)
{
    reader_saved_position_t saved;
    if (!reader_read_saved_position(&saved)) {
        return;
    }
    if (saved.zoom >= 0 && saved.zoom <= READER_MAX_ZOOM) {
        reader.zoom = saved.zoom;
    }
    reader.layout_valid = false;
    reader_scroll_to_anchor(ctx, saved.offset);
    snprintf(reader.message,
             sizeof(reader.message),
             saved.len == reader.doc.source_len ? "resumed" : "resumed, chapter changed");
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
            "%" PRIu64 " %" PRIu64 " z=%d c=%u %s\n",
            (uint64_t)offset,
            (uint64_t)reader.doc.source_len,
            reader.zoom,
            (unsigned)(reader.epub ? reader.epub_chapter : 0U),
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

    if (reader.search_input) {
        snprintf(title, sizeof(title), "/%s_", reader.search);
    } else if (reader.message[0] != '\0') {
        snprintf(title, sizeof(title), "reader z%d %s", reader.zoom, reader.message);
    } else if (reader.epub && reader.epub_chapter_count > 0) {
        snprintf(title,
                 sizeof(title),
                 "reader z%d %u/%u %s",
                 reader.zoom,
                 (unsigned)(reader.epub_chapter + 1U),
                 (unsigned)reader.epub_chapter_count,
                 reader.display_name);
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
        if (reader.epub && reader.epub_chapter > 0) {
            (void)reader_switch_epub_chapter(ctx, reader.epub_chapter - 1U, true);
            return;
        }
        target = 0;
    } else if (target >= (int)reader.layout.line_count) {
        if (reader.epub && reader.epub_chapter + 1U < reader.epub_chapter_count) {
            (void)reader_switch_epub_chapter(ctx, reader.epub_chapter + 1U, false);
            return;
        }
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

static bool reader_match_to_y(int *out_y, int *out_height)
{
    if (!reader.match.valid || !reader.layout_valid) {
        return false;
    }

    const size_t match_start = reader.match.text_offset;
    const size_t match_end = reader.match.text_offset + reader.match.text_len;
    for (size_t i = 0; i < reader.layout.run_count; i++) {
        const solar_os_doc_layout_run_t *run = &reader.layout.runs[i];
        if (run->block_index != reader.match.block_index || run->text_offset == SIZE_MAX) {
            continue;
        }
        const size_t run_start = run->text_offset;
        const size_t run_end = run->text_offset + run->text_len;
        if (match_start < run_end && match_end > run_start) {
            if (out_y != NULL) {
                *out_y = run->y;
            }
            if (out_height != NULL) {
                *out_height = run->height;
            }
            return true;
        }
    }

    for (size_t i = 0; i < reader.layout.line_count; i++) {
        const solar_os_doc_layout_line_t *line = &reader.layout.lines[i];
        if (line->block_index == reader.match.block_index) {
            if (out_y != NULL) {
                *out_y = line->y;
            }
            if (out_height != NULL) {
                *out_height = line->height;
            }
            return true;
        }
    }
    return false;
}

static void reader_scroll_to_match(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    reader_update_measure(gfx);

    int y = 0;
    int height = 0;
    if (!reader_match_to_y(&y, &height)) {
        return;
    }

    reader.scroll_y = y - READER_MARGIN_Y;
    reader_clamp_scroll(gfx);
}

static bool reader_run_overlaps_match(const solar_os_doc_layout_run_t *run,
                                      size_t *overlap_start,
                                      size_t *overlap_end)
{
    if (run == NULL ||
        !reader.match.valid ||
        run->block_index != reader.match.block_index ||
        run->text_offset == SIZE_MAX ||
        run->text_len == 0) {
        return false;
    }

    const size_t match_start = reader.match.text_offset;
    const size_t match_end = reader.match.text_offset + reader.match.text_len;
    const size_t run_start = run->text_offset;
    const size_t run_end = run->text_offset + run->text_len;
    if (match_start >= run_end || match_end <= run_start) {
        return false;
    }

    if (overlap_start != NULL) {
        *overlap_start = match_start > run_start ? match_start : run_start;
    }
    if (overlap_end != NULL) {
        *overlap_end = match_end < run_end ? match_end : run_end;
    }
    return true;
}

static void reader_draw_match_text(solar_os_gfx_t *gfx,
                                   const char *text,
                                   size_t len,
                                   int x,
                                   int y,
                                   int baseline,
                                   int height,
                                   int char_w,
                                   int clip_x,
                                   int clip_w)
{
    char temp[96];

    if (gfx == NULL || text == NULL || len == 0 || char_w <= 0 || clip_w <= 0) {
        return;
    }

    const int clip_right = clip_x + clip_w;
    if (x >= clip_right || x + ((int)len * char_w) <= clip_x) {
        return;
    }

    size_t skip = 0;
    if (x < clip_x) {
        skip = (size_t)((clip_x - x + char_w - 1) / char_w);
        if (skip > len) {
            return;
        }
        x += (int)skip * char_w;
        text += skip;
        len -= skip;
    }

    size_t max_chars = (size_t)((clip_right - x) / char_w);
    if (max_chars > len) {
        max_chars = len;
    }
    if (max_chars >= sizeof(temp)) {
        max_chars = sizeof(temp) - 1U;
    }
    if (max_chars == 0) {
        return;
    }

    memcpy(temp, text, max_chars);
    temp[max_chars] = '\0';
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, x - 1, y, (int)max_chars * char_w + 2, height);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_text(gfx, x, y + baseline, temp);
}

static void reader_draw_match(solar_os_gfx_t *gfx, const solar_os_doc_view_t *view)
{
    if (gfx == NULL || view == NULL || !reader.match.valid || !reader.layout_valid) {
        return;
    }
    if (reader.match.block_index >= reader.doc.block_count) {
        return;
    }

    const solar_os_doc_block_t *block = &reader.doc.blocks[reader.match.block_index];
    const char *block_text = block->text != NULL ? block->text : "";
    const size_t block_len = strlen(block_text);
    if (reader.match.text_offset >= block_len) {
        return;
    }

    for (size_t i = 0; i < reader.layout.run_count; i++) {
        const solar_os_doc_layout_run_t *run = &reader.layout.runs[i];
        size_t start = 0;
        size_t end = 0;
        if (!reader_run_overlaps_match(run, &start, &end) || start >= end || end > block_len) {
            continue;
        }

        const int screen_y = view->y + run->y - view->scroll_y;
        if (screen_y + run->height < view->y || screen_y > view->y + view->height) {
            continue;
        }

        const int char_w = run->char_w > 0 ? run->char_w : 1;
        const int screen_x = view->x + run->x + ((int)(start - run->text_offset) * char_w);

        solar_os_gfx_set_font(gfx, run->font);
        reader_draw_match_text(gfx,
                               &block_text[start],
                               end - start,
                               screen_x,
                               screen_y,
                               run->baseline,
                               run->height,
                               char_w,
                               view->x,
                               view->width);
    }
}

static bool reader_run_search(solar_os_context_t *ctx, bool forward, bool next)
{
    if (reader.last_search_len == 0) {
        snprintf(reader.message, sizeof(reader.message), "no search");
        reader.search_status = true;
        reader.match.valid = false;
        return false;
    }

    reader_update_measure(solar_os_context_gfx(ctx));

    size_t start_block = 0;
    size_t start_offset = 0;
    if (next && reader.match.valid) {
        start_block = reader.match.block_index;
        start_offset = reader.match.text_offset;
        if (forward) {
            start_offset++;
        }
    } else {
        (void)reader_current_search_position(&start_block, &start_offset);
    }

    bool wrapped = false;
    if (!reader_find_text(reader.last_search, reader.last_search_len, start_block, start_offset, forward, &wrapped)) {
        snprintf(reader.message, sizeof(reader.message), "not found: %s", reader.last_search);
        reader.search_status = true;
        return false;
    }

    reader_scroll_to_match(ctx);
    snprintf(reader.message,
             sizeof(reader.message),
             "%s%s",
             wrapped ? "wrapped: " : "found: ",
             reader.last_search);
    reader.search_status = true;
    return true;
}

static void reader_start_search(void)
{
    reader.search_input = true;
    reader.search_status = false;
    reader.search_len = 0;
    reader.search[0] = '\0';
    reader.message[0] = '\0';
}

static void reader_cancel_search(void)
{
    reader.search_input = false;
    reader.search_len = 0;
    reader.search[0] = '\0';
}

static void reader_submit_search(solar_os_context_t *ctx)
{
    reader.search_input = false;
    if (reader.search_len > 0) {
        strlcpy(reader.last_search, reader.search, sizeof(reader.last_search));
        reader.last_search_len = strlen(reader.last_search);
    }
    reader.search_len = 0;
    reader.search[0] = '\0';
    (void)reader_run_search(ctx, true, false);
}

static bool reader_handle_search_input(solar_os_context_t *ctx, uint8_t ch)
{
    switch (ch) {
    case SOLAR_OS_KEY_APP_EXIT:
        solar_os_context_request_exit(ctx);
        break;
    case SOLAR_OS_KEY_ESCAPE:
        reader_cancel_search();
        break;
    case '\r':
    case '\n':
        reader_submit_search(ctx);
        break;
    case '\b':
    case 0x7f:
        if (reader.search_len > 0) {
            reader.search[--reader.search_len] = '\0';
        }
        break;
    default:
        if (reader_is_printable(ch) && reader.search_len + 1U < sizeof(reader.search)) {
            reader.search[reader.search_len++] = (char)ch;
            reader.search[reader.search_len] = '\0';
        }
        break;
    }
    return true;
}

static void reader_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || reader.suspended) {
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
            reader_draw_match(gfx, &view);
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
    if (reader_path_is_epub(path)) {
        return reader_load_epub_chapter(reader.epub_chapter);
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
    reader.suspended = false;

    if (solar_os_context_gfx(ctx) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const int argc = solar_os_context_argc(ctx);
    if (argc != 2) {
        reader.error_only = true;
        snprintf(reader.message, sizeof(reader.message), "usage: reader <file.txt|file.md|file.epub>");
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

    reader_saved_position_t saved;
    const bool has_saved = reader_read_saved_position(&saved);
    if (has_saved && saved.zoom >= 0 && saved.zoom <= READER_MAX_ZOOM) {
        reader.zoom = saved.zoom;
    }

    reader.epub = reader_path_is_epub(reader.path);
    if (reader.epub) {
        err = solar_os_epub_open(reader.path, &reader.epub_book);
        if (err != ESP_OK) {
            reader.error_only = true;
            snprintf(reader.message, sizeof(reader.message), "epub open failed: %s", esp_err_to_name(err));
            solar_os_context_set_graphics_active(ctx, true);
            reader_render(ctx);
            return ESP_OK;
        }
        reader.epub_chapter_count = solar_os_epub_spine_count(reader.epub_book);
        if (reader.epub_chapter_count == 0) {
            reader.error_only = true;
            snprintf(reader.message, sizeof(reader.message), "epub has no readable spine");
            solar_os_context_set_graphics_active(ctx, true);
            reader_render(ctx);
            return ESP_OK;
        }
        if (has_saved && saved.chapter < reader.epub_chapter_count) {
            reader.epub_chapter = saved.chapter;
        }
        const char *title = solar_os_epub_title(reader.epub_book);
        if (title != NULL && title[0] != '\0') {
            strlcpy(reader.display_name, title, sizeof(reader.display_name));
        }
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
    if (has_saved) {
        reader.layout_valid = false;
        reader_scroll_to_anchor(ctx, saved.offset);
        if (reader.epub) {
            snprintf(reader.message,
                     sizeof(reader.message),
                     "resumed chapter %u/%u",
                     (unsigned)(reader.epub_chapter + 1U),
                     (unsigned)reader.epub_chapter_count);
        } else {
            snprintf(reader.message, sizeof(reader.message), "resumed");
        }
    } else if (!reader.epub) {
        reader_load_position(ctx);
    }
    reader_render(ctx);
    return ESP_OK;
}

static void reader_stop(solar_os_context_t *ctx)
{
    reader_save_position(ctx);
    solar_os_doc_free(&reader.doc);
    solar_os_doc_layout_free(&reader.layout);
    solar_os_epub_close(reader.epub_book);
    memset(&reader, 0, sizeof(reader));
    solar_os_context_set_graphics_active(ctx, false);
}

static void reader_suspend(solar_os_context_t *ctx)
{
    reader_save_position(ctx);
    reader.suspended = true;
    solar_os_context_set_graphics_active(ctx, false);
}

static void reader_resume(solar_os_context_t *ctx)
{
    reader.suspended = false;
    solar_os_context_set_graphics_active(ctx, true);
    reader_render(ctx);
}

static void reader_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    if (reader.display_name[0] != '\0') {
        snprintf(buffer, buffer_len, "reader %s", reader.display_name);
        return;
    }
    strlcpy(buffer, "reader", buffer_len);
}

static void reader_page(solar_os_context_t *ctx, bool down)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    reader_update_measure(gfx);
    if (!reader.layout_valid || reader.layout.line_count == 0) {
        return;
    }

    const int max_scroll = reader_max_scroll(gfx);
    if (reader.epub && down && reader.scroll_y >= max_scroll) {
        if (reader.epub_chapter + 1U < reader.epub_chapter_count) {
            (void)reader_switch_epub_chapter(ctx, reader.epub_chapter + 1U, false);
        }
        return;
    }
    if (reader.epub && !down && reader.scroll_y <= 0) {
        if (reader.epub_chapter > 0) {
            (void)reader_switch_epub_chapter(ctx, reader.epub_chapter - 1U, true);
        }
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

    if (reader.search_input) {
        return reader_handle_search_input(ctx, ch);
    }

    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == 'q' || ch == 'Q') {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (reader.error_only) {
        if (ch == SOLAR_OS_KEY_ESCAPE) {
            solar_os_context_request_exit(ctx);
        }
        return true;
    }

    switch (ch) {
    case SOLAR_OS_KEY_ESCAPE:
        if (reader.match.valid || reader.search_status) {
            reader.match.valid = false;
            reader.search_status = false;
            reader.message[0] = '\0';
        } else {
            solar_os_context_request_exit(ctx);
        }
        break;
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
    case '/':
        reader_start_search();
        break;
    case 'n':
        (void)reader_run_search(ctx, true, true);
        break;
    case 'N':
        (void)reader_run_search(ctx, false, true);
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
        reader_resume(ctx);
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
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = reader_start,
    .suspend = reader_suspend,
    .resume = reader_resume,
    .stop = reader_stop,
    .event = reader_event,
    .title = reader_title,
};
