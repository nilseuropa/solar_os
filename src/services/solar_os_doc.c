#include "solar_os_doc.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "solar_os_config.h"
#include "solar_os_log.h"

#if SOLAR_OS_PACKAGE_MEDIA || SOLAR_OS_PACKAGE_NET
#include "solar_os_stb_image.h"
#include "solar_os_webp_decoder.h"
#endif

#define DOC_INITIAL_BLOCKS 32U
#define DOC_INITIAL_RUNS 32U
#define DOC_MAX_BYTES (2U * 1024U * 1024U)
#define DOC_MAX_IMAGE_BYTES (1024U * 1024U)
#define DOC_MAX_IMAGE_PIXELS (1024U * 1024U)
#define DOC_TAB_WIDTH 4
#define DOC_TEXT_SOFT_LINE_MIN 40U
#define DOC_TABLE_CELL_PAD 3

static const char *TAG = "solar_os_doc";

typedef struct {
    solar_os_gfx_font_t font;
    int char_w;
    int line_h;
    int baseline;
    int paragraph_gap;
    int heading_gap;
    int block_gap;
    uint16_t pixel_size;
    uint8_t face;
} doc_style_t;

static void *doc_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void *doc_calloc(size_t count, size_t size)
{
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void *doc_realloc(void *ptr, size_t size)
{
    void *next = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (next == NULL) {
        next = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    }
    return next;
}

static void doc_free(void *ptr)
{
    heap_caps_free(ptr);
}

void solar_os_doc_init(solar_os_doc_t *doc)
{
    if (doc != NULL) {
        memset(doc, 0, sizeof(*doc));
    }
}

static void doc_clear(solar_os_doc_t *doc, bool keep_assets)
{
    if (doc == NULL) {
        return;
    }

    solar_os_doc_asset_provider_t assets = doc->assets;
    for (size_t i = 0; i < doc->block_count; i++) {
        doc_free(doc->blocks[i].text);
        doc_free(doc->blocks[i].image.gray);
    }
    doc_free(doc->blocks);
    doc_free(doc->runs);
    doc_free(doc->source);
    memset(doc, 0, sizeof(*doc));
    if (keep_assets) {
        doc->assets = assets;
    }
}

void solar_os_doc_free(solar_os_doc_t *doc)
{
    doc_clear(doc, false);
}

void solar_os_doc_set_asset_provider(solar_os_doc_t *doc,
                                     const solar_os_doc_asset_provider_t *provider)
{
    if (doc == NULL) {
        return;
    }

    if (provider != NULL) {
        doc->assets = *provider;
    } else {
        memset(&doc->assets, 0, sizeof(doc->assets));
    }
}

static bool doc_path_has_suffix(const char *path, const char *suffix)
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

static void doc_release_asset(const solar_os_doc_t *doc, uint8_t *data)
{
    if (data == NULL) {
        return;
    }
    if (doc != NULL && doc->assets.release != NULL) {
        doc->assets.release(doc->assets.user, data);
        return;
    }

    doc_free(data);
}

static esp_err_t doc_decode_image_asset(solar_os_doc_t *doc,
                                        const char *target,
                                        solar_os_doc_image_t *image)
{
    if (doc == NULL || target == NULL || image == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    image->attempted = true;
    if (doc->assets.read == NULL) {
        image->status = ESP_ERR_NOT_FOUND;
        return image->status;
    }

    uint8_t *bytes = NULL;
    size_t bytes_len = 0;
    esp_err_t ret = doc->assets.read(doc->assets.user,
                                     doc->path,
                                     target,
                                     &bytes,
                                     &bytes_len);
    if (ret != ESP_OK) {
        image->status = ret;
        return ret;
    }
    if (bytes == NULL || bytes_len == 0 || bytes_len > DOC_MAX_IMAGE_BYTES) {
        doc_release_asset(doc, bytes);
        image->status = ESP_ERR_INVALID_SIZE;
        return image->status;
    }

#if SOLAR_OS_PACKAGE_MEDIA || SOLAR_OS_PACKAGE_NET
    uint8_t *gray = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    if (doc_path_has_suffix(target, ".webp")) {
        ret = solar_os_webp_decode_gray(bytes,
                                        bytes_len,
                                        DOC_MAX_IMAGE_PIXELS,
                                        &gray,
                                        &width,
                                        &height);
    } else {
        ret = solar_os_stb_decode_gray(bytes,
                                       bytes_len,
                                       DOC_MAX_IMAGE_PIXELS,
                                       &gray,
                                       &width,
                                       &height);
    }
    doc_release_asset(doc, bytes);

    image->status = ret;
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "image decode failed: %s %s", target, esp_err_to_name(ret));
        return ret;
    }
    image->gray = gray;
    image->width = width;
    image->height = height;
    return ESP_OK;
#else
    doc_release_asset(doc, bytes);
    image->status = ESP_ERR_NOT_SUPPORTED;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t doc_reserve_blocks(solar_os_doc_t *doc, size_t need)
{
    if (doc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (doc->block_capacity >= need) {
        return ESP_OK;
    }

    size_t next = doc->block_capacity > 0 ? doc->block_capacity * 2U : DOC_INITIAL_BLOCKS;
    while (next < need) {
        next *= 2U;
    }
    solar_os_doc_block_t *blocks = doc_calloc(next, sizeof(blocks[0]));
    if (blocks == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (doc->blocks != NULL && doc->block_count > 0) {
        memcpy(blocks, doc->blocks, doc->block_count * sizeof(blocks[0]));
    }
    doc_free(doc->blocks);
    doc->blocks = blocks;
    doc->block_capacity = next;
    return ESP_OK;
}

static esp_err_t doc_reserve_runs(solar_os_doc_t *doc, size_t need)
{
    if (doc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (doc->run_capacity >= need) {
        return ESP_OK;
    }

    size_t next = doc->run_capacity > 0 ? doc->run_capacity * 2U : DOC_INITIAL_RUNS;
    while (next < need) {
        next *= 2U;
    }
    solar_os_doc_run_t *runs = doc_calloc(next, sizeof(runs[0]));
    if (runs == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (doc->runs != NULL && doc->run_count > 0) {
        memcpy(runs, doc->runs, doc->run_count * sizeof(runs[0]));
    }
    doc_free(doc->runs);
    doc->runs = runs;
    doc->run_capacity = next;
    return ESP_OK;
}

static char *doc_copy_range(const char *source, size_t start, size_t end)
{
    if (source == NULL || end < start) {
        return NULL;
    }

    char *copy = doc_malloc((end - start) + 1U);
    if (copy == NULL) {
        return NULL;
    }
    if (end > start) {
        memcpy(copy, &source[start], end - start);
    }
    copy[end - start] = '\0';
    return copy;
}

static char *doc_copy_text(const char *text)
{
    const size_t len = text != NULL ? strlen(text) : 0;
    char *copy = doc_malloc(len + 1U);
    if (copy == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(copy, text, len);
    }
    copy[len] = '\0';
    return copy;
}

static esp_err_t doc_add_run(solar_os_doc_t *doc,
                             size_t text_offset,
                             size_t text_len,
                             size_t source_start,
                             size_t source_end,
                             uint8_t style)
{
    esp_err_t ret = doc_reserve_runs(doc, doc->run_count + 1U);
    if (ret != ESP_OK) {
        return ret;
    }

    doc->runs[doc->run_count++] = (solar_os_doc_run_t){
        .text_offset = text_offset,
        .text_len = text_len,
        .source_start = source_start,
        .source_end = source_end,
        .style = style,
    };
    return ESP_OK;
}

static esp_err_t doc_append_inline_text(char **out,
                                        size_t *out_len,
                                        size_t *out_cap,
                                        const char *text,
                                        size_t start,
                                        size_t end)
{
    if (out == NULL || out_len == NULL || out_cap == NULL || text == NULL || end < start) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t add_len = end - start;
    if (*out_len + add_len + 1U > *out_cap) {
        size_t next = *out_cap > 0 ? *out_cap * 2U : 128U;
        while (next < *out_len + add_len + 1U) {
            next *= 2U;
        }
        char *buf = doc_malloc(next);
        if (buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        if (*out != NULL && *out_len > 0) {
            memcpy(buf, *out, *out_len);
        }
        doc_free(*out);
        *out = buf;
        *out_cap = next;
    }

    if (add_len > 0) {
        memcpy(&(*out)[*out_len], &text[start], add_len);
        *out_len += add_len;
    }
    (*out)[*out_len] = '\0';
    return ESP_OK;
}

static esp_err_t doc_inline_flush_segment(solar_os_doc_t *doc,
                                          solar_os_doc_block_t *block,
                                          char **out,
                                          size_t *out_len,
                                          size_t *out_cap,
                                          const char *text,
                                          size_t start,
                                          size_t end,
                                          size_t source_base,
                                          uint8_t style)
{
    if (end <= start) {
        return ESP_OK;
    }

    const size_t text_offset = *out_len;
    esp_err_t ret = doc_append_inline_text(out, out_len, out_cap, text, start, end);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = doc_add_run(doc,
                      text_offset,
                      end - start,
                      source_base + start,
                      source_base + end,
                      style);
    if (ret != ESP_OK) {
        return ret;
    }
    block->run_count++;
    return ESP_OK;
}

static size_t doc_find_marker(const char *text, size_t len, size_t start, const char *marker)
{
    const size_t marker_len = marker != NULL ? strlen(marker) : 0;
    if (text == NULL || marker_len == 0 || start >= len) {
        return SIZE_MAX;
    }

    for (size_t i = start; i + marker_len <= len; i++) {
        if (memcmp(&text[i], marker, marker_len) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static esp_err_t doc_parse_inline_block(solar_os_doc_t *doc,
                                        solar_os_doc_block_t *block,
                                        char *raw_text,
                                        size_t source_base,
                                        uint8_t default_style)
{
    if (doc == NULL || block == NULL || raw_text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t raw_len = strlen(raw_text);
    if ((default_style & SOLAR_OS_DOC_RUN_CODE) != 0) {
        block->text = raw_text;
        const esp_err_t ret = doc_add_run(doc,
                                          0,
                                          raw_len,
                                          source_base,
                                          source_base + raw_len,
                                          default_style);
        if (ret == ESP_OK) {
            block->run_count = raw_len > 0 ? 1U : 0U;
        }
        return ret;
    }

    char *out = NULL;
    size_t out_len = 0;
    size_t out_cap = 0;
    size_t segment_start = 0;
    size_t i = 0;

    while (i < raw_len) {
        const char ch = raw_text[i];
        const bool can_parse_marker = i == 0 || !isalnum((unsigned char)raw_text[i - 1U]);
        const char *marker = NULL;
        size_t marker_len = 0;
        uint8_t marker_style = 0;
        size_t content_start = 0;
        size_t content_end = SIZE_MAX;
        size_t token_end = 0;

        if (ch == '[') {
            const size_t close_text = doc_find_marker(raw_text, raw_len, i + 1U, "](");
            if (close_text != SIZE_MAX) {
                const size_t close_url = doc_find_marker(raw_text, raw_len, close_text + 2U, ")");
                if (close_url != SIZE_MAX) {
                    content_start = i + 1U;
                    content_end = close_text;
                    token_end = close_url + 1U;
                    marker_style = SOLAR_OS_DOC_RUN_LINK | SOLAR_OS_DOC_RUN_UNDERLINE;
                }
            }
        } else if (ch == '`') {
            marker = "`";
            marker_len = 1U;
            marker_style = SOLAR_OS_DOC_RUN_CODE;
        } else if (i + 1U < raw_len &&
                   ((raw_text[i] == '*' && raw_text[i + 1U] == '*') ||
                    (raw_text[i] == '_' && raw_text[i + 1U] == '_'))) {
            marker = raw_text[i] == '*' ? "**" : "__";
            marker_len = 2U;
            marker_style = SOLAR_OS_DOC_RUN_BOLD;
        } else if (can_parse_marker && (ch == '*' || ch == '_')) {
            marker = ch == '*' ? "*" : "_";
            marker_len = 1U;
            marker_style = SOLAR_OS_DOC_RUN_ITALIC;
        }

        if (marker != NULL) {
            const size_t close = doc_find_marker(raw_text, raw_len, i + marker_len, marker);
            if (close != SIZE_MAX) {
                content_start = i + marker_len;
                content_end = close;
                token_end = close + marker_len;
            }
        }

        if (content_end == SIZE_MAX || content_end < content_start || token_end <= i) {
            i++;
            continue;
        }

        esp_err_t ret = doc_inline_flush_segment(doc,
                                                 block,
                                                 &out,
                                                 &out_len,
                                                 &out_cap,
                                                 raw_text,
                                                 segment_start,
                                                 i,
                                                 source_base,
                                                 default_style);
        if (ret != ESP_OK) {
            doc_free(out);
            return ret;
        }
        ret = doc_inline_flush_segment(doc,
                                       block,
                                       &out,
                                       &out_len,
                                       &out_cap,
                                       raw_text,
                                       content_start,
                                       content_end,
                                       source_base,
                                       default_style | marker_style);
        if (ret != ESP_OK) {
            doc_free(out);
            return ret;
        }
        i = token_end;
        segment_start = i;
    }

    esp_err_t ret = doc_inline_flush_segment(doc,
                                             block,
                                             &out,
                                             &out_len,
                                             &out_cap,
                                             raw_text,
                                             segment_start,
                                             raw_len,
                                             source_base,
                                             default_style);
    if (ret != ESP_OK) {
        doc_free(out);
        return ret;
    }

    if (out == NULL) {
        out = doc_copy_text("");
        if (out == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    doc_free(raw_text);
    block->text = out;
    return ESP_OK;
}

static esp_err_t doc_add_block(solar_os_doc_t *doc,
                               solar_os_doc_block_type_t type,
                               uint8_t level,
                               size_t source_start,
                               size_t source_end,
                               char *text,
                               uint8_t run_style)
{
    esp_err_t ret = doc_reserve_blocks(doc, doc->block_count + 1U);
    if (ret != ESP_OK) {
        doc_free(text);
        return ret;
    }

    const size_t block_index = doc->block_count++;
    solar_os_doc_block_t *block = &doc->blocks[block_index];
    *block = (solar_os_doc_block_t){
        .type = type,
        .level = level,
        .source_start = source_start,
        .source_end = source_end,
        .run_start = doc->run_count,
        .run_count = 0,
        .text = text,
    };

    const size_t text_len = text != NULL ? strlen(text) : 0;
    if (text_len > 0) {
        ret = doc_parse_inline_block(doc, block, text, source_start, run_style);
        if (ret != ESP_OK) {
            doc_free(block->text);
            block->text = NULL;
            doc->block_count--;
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t doc_add_plain_block(solar_os_doc_t *doc,
                                     solar_os_doc_block_type_t type,
                                     uint8_t level,
                                     size_t source_start,
                                     size_t source_end,
                                     char *text,
                                     uint8_t style)
{
    esp_err_t ret = doc_reserve_blocks(doc, doc->block_count + 1U);
    if (ret != ESP_OK) {
        doc_free(text);
        return ret;
    }

    const size_t block_index = doc->block_count++;
    solar_os_doc_block_t *block = &doc->blocks[block_index];
    *block = (solar_os_doc_block_t){
        .type = type,
        .level = level,
        .source_start = source_start,
        .source_end = source_end,
        .run_start = doc->run_count,
        .run_count = 0,
        .text = text,
    };

    const size_t text_len = text != NULL ? strlen(text) : 0;
    if (text_len == 0) {
        return ESP_OK;
    }

    ret = doc_add_run(doc, 0, text_len, source_start, source_end, style);
    if (ret != ESP_OK) {
        doc_free(block->text);
        block->text = NULL;
        doc->block_count--;
        return ret;
    }
    block->run_count = 1;
    return ESP_OK;
}

static esp_err_t doc_add_empty_block(solar_os_doc_t *doc,
                                     solar_os_doc_block_type_t type,
                                     uint8_t level,
                                     size_t source_start,
                                     size_t source_end)
{
    esp_err_t ret = doc_reserve_blocks(doc, doc->block_count + 1U);
    if (ret != ESP_OK) {
        return ret;
    }

    doc->blocks[doc->block_count++] = (solar_os_doc_block_t){
        .type = type,
        .level = level,
        .source_start = source_start,
        .source_end = source_end,
        .run_start = doc->run_count,
        .run_count = 0,
        .text = NULL,
    };
    return ESP_OK;
}

static esp_err_t doc_add_image_block(solar_os_doc_t *doc,
                                     size_t source_start,
                                     size_t source_end,
                                     char *target)
{
    esp_err_t ret = doc_reserve_blocks(doc, doc->block_count + 1U);
    if (ret != ESP_OK) {
        doc_free(target);
        return ret;
    }

    solar_os_doc_block_t *block = &doc->blocks[doc->block_count++];
    *block = (solar_os_doc_block_t){
        .type = SOLAR_OS_DOC_BLOCK_IMAGE,
        .level = 0,
        .source_start = source_start,
        .source_end = source_end,
        .run_start = doc->run_count,
        .run_count = 0,
        .text = target,
    };

    (void)doc_decode_image_asset(doc, target, &block->image);
    return ESP_OK;
}

static size_t doc_line_end(const char *source, size_t len, size_t offset)
{
    while (offset < len && source[offset] != '\n' && source[offset] != '\r') {
        offset++;
    }
    return offset;
}

static size_t doc_next_line(const char *source, size_t len, size_t offset)
{
    const size_t end = doc_line_end(source, len, offset);
    if (end >= len) {
        return len;
    }
    if (source[end] == '\r' && end + 1U < len && source[end + 1U] == '\n') {
        return end + 2U;
    }
    return end + 1U;
}

static size_t doc_skip_space(const char *source, size_t start, size_t end)
{
    while (start < end && (source[start] == ' ' || source[start] == '\t')) {
        start++;
    }
    return start;
}

static bool doc_line_blank(const char *source, size_t start, size_t end)
{
    for (size_t i = start; i < end; i++) {
        if (source[i] != ' ' && source[i] != '\t') {
            return false;
        }
    }
    return true;
}

static bool doc_parse_heading(const char *source,
                              size_t start,
                              size_t end,
                              uint8_t *level,
                              size_t *text_start)
{
    size_t p = doc_skip_space(source, start, end);
    uint8_t count = 0;
    while (p < end && source[p] == '#' && count < 6U) {
        count++;
        p++;
    }
    if (count == 0 || p >= end) {
        return false;
    }
    if (source[p] == ' ' || source[p] == '\t') {
        p++;
    }
    *level = count;
    *text_start = p;
    return true;
}

static bool doc_parse_list(const char *source, size_t start, size_t end, size_t *text_start)
{
    size_t p = doc_skip_space(source, start, end);
    if (p >= end) {
        return false;
    }

    const char first = source[p];
    if ((first == '-' || first == '*' || first == '+') &&
        p + 1U < end &&
        (source[p + 1U] == ' ' || source[p + 1U] == '\t')) {
        *text_start = p + 2U;
        return true;
    }
    if (isdigit((unsigned char)first)) {
        size_t q = p + 1U;
        while (q < end && isdigit((unsigned char)source[q])) {
            q++;
        }
        if (q + 1U < end &&
            (source[q] == '.' || source[q] == ')') &&
            (source[q + 1U] == ' ' || source[q + 1U] == '\t')) {
            *text_start = q + 2U;
            return true;
        }
    }
    return false;
}

static bool doc_parse_quote(const char *source, size_t start, size_t end, size_t *text_start)
{
    size_t p = doc_skip_space(source, start, end);
    if (p < end && source[p] == '>') {
        p++;
        if (p < end && (source[p] == ' ' || source[p] == '\t')) {
            p++;
        }
        *text_start = p;
        return true;
    }
    return false;
}

static bool doc_parse_fence(const char *source, size_t start, size_t end)
{
    const size_t p = doc_skip_space(source, start, end);
    return p + 2U < end && source[p] == '`' && source[p + 1U] == '`' && source[p + 2U] == '`';
}

static size_t doc_trim_start(const char *source, size_t start, size_t end);
static size_t doc_trim_end(const char *source, size_t start, size_t end);

static bool doc_parse_rule(const char *source, size_t start, size_t end)
{
    start = doc_trim_start(source, start, end);
    end = doc_trim_end(source, start, end);
    if (end - start < 3U) {
        return false;
    }

    const char marker = source[start];
    if (marker != '-' && marker != '*' && marker != '_') {
        return false;
    }

    size_t count = 0;
    for (size_t i = start; i < end; i++) {
        if (source[i] == marker) {
            count++;
            continue;
        }
        if (source[i] != ' ' && source[i] != '\t') {
            return false;
        }
    }
    return count >= 3U;
}

static bool doc_line_is_indented_code(const char *source, size_t start, size_t end)
{
    if (start >= end) {
        return false;
    }
    if (source[start] == '\t') {
        return true;
    }
    size_t spaces = 0;
    while (start + spaces < end && source[start + spaces] == ' ') {
        spaces++;
    }
    return spaces >= 4U;
}

static size_t doc_indented_code_text_start(const char *source, size_t start, size_t end)
{
    if (start < end && source[start] == '\t') {
        return start + 1U;
    }
    size_t spaces = 0;
    while (spaces < 4U && start < end && source[start] == ' ') {
        spaces++;
        start++;
    }
    return start;
}

static bool doc_line_has_table_pipe(const char *source, size_t start, size_t end)
{
    for (size_t i = start; i < end; i++) {
        if (source[i] == '|') {
            return true;
        }
    }
    return false;
}

static bool doc_parse_table_delimiter(const char *source, size_t start, size_t end)
{
    start = doc_trim_start(source, start, end);
    end = doc_trim_end(source, start, end);
    if (end <= start || !doc_line_has_table_pipe(source, start, end)) {
        return false;
    }

    bool saw_dash = false;
    for (size_t i = start; i < end; i++) {
        const char ch = source[i];
        if (ch == '-') {
            saw_dash = true;
            continue;
        }
        if (ch != '|' && ch != ':' && ch != ' ' && ch != '\t') {
            return false;
        }
    }
    return saw_dash;
}

static char *doc_copy_table_row(const char *source, size_t start, size_t end)
{
    start = doc_trim_start(source, start, end);
    end = doc_trim_end(source, start, end);
    if (start < end && source[start] == '|') {
        start++;
    }
    if (end > start && source[end - 1U] == '|') {
        end--;
    }

    return doc_copy_range(source, start, end);
}

static bool doc_parse_image_line(const char *source,
                                 size_t start,
                                 size_t end,
                                 size_t *target_start,
                                 size_t *target_end)
{
    start = doc_trim_start(source, start, end);
    end = doc_trim_end(source, start, end);
    if (target_start == NULL || target_end == NULL || start + 4U >= end ||
        source[start] != '!' || source[start + 1U] != '[') {
        return false;
    }

    const size_t close_alt = doc_find_marker(source, end, start + 2U, "](");
    if (close_alt == SIZE_MAX) {
        return false;
    }
    const size_t close_target = doc_find_marker(source, end, close_alt + 2U, ")");
    if (close_target == SIZE_MAX || close_target + 1U != end) {
        return false;
    }

    *target_start = doc_trim_start(source, close_alt + 2U, close_target);
    *target_end = doc_trim_end(source, *target_start, close_target);
    return *target_end > *target_start;
}

static esp_err_t doc_append_paragraph_text(char **text,
                                           size_t *len,
                                           size_t *cap,
                                           const char *source,
                                           size_t start,
                                           size_t end)
{
    if (end <= start) {
        return ESP_OK;
    }

    const size_t add_space = *len > 0 ? 1U : 0U;
    const size_t add_len = (end - start) + add_space;
    if (*len + add_len + 1U > *cap) {
        size_t next = *cap > 0 ? *cap * 2U : 128U;
        while (next < *len + add_len + 1U) {
            next *= 2U;
        }
        char *buf = doc_malloc(next);
        if (buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        if (*text != NULL && *len > 0) {
            memcpy(buf, *text, *len);
        }
        doc_free(*text);
        *text = buf;
        *cap = next;
    }
    if (add_space) {
        (*text)[(*len)++] = ' ';
    }
    memcpy(&(*text)[*len], &source[start], end - start);
    *len += end - start;
    (*text)[*len] = '\0';
    return ESP_OK;
}

static esp_err_t doc_append_raw_text(char **text,
                                     size_t *len,
                                     size_t *cap,
                                     const char *source,
                                     size_t start,
                                     size_t end,
                                     bool add_newline)
{
    if (text == NULL || len == NULL || cap == NULL || source == NULL || end < start) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t add_len = (end - start) + (add_newline ? 1U : 0U);
    if (*len + add_len + 1U > *cap) {
        size_t next = *cap > 0 ? *cap * 2U : 128U;
        while (next < *len + add_len + 1U) {
            next *= 2U;
        }
        char *buf = doc_malloc(next);
        if (buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        if (*text != NULL && *len > 0) {
            memcpy(buf, *text, *len);
        }
        doc_free(*text);
        *text = buf;
        *cap = next;
    }

    if (end > start) {
        memcpy(&(*text)[*len], &source[start], end - start);
        *len += end - start;
    }
    if (add_newline) {
        (*text)[(*len)++] = '\n';
    }
    (*text)[*len] = '\0';
    return ESP_OK;
}

static esp_err_t doc_flush_paragraph(solar_os_doc_t *doc,
                                     char **text,
                                     size_t *len,
                                     size_t *cap,
                                     size_t source_start,
                                     size_t source_end)
{
    if (*text == NULL || *len == 0) {
        return ESP_OK;
    }
    char *owned = *text;
    *text = NULL;
    *len = 0;
    *cap = 0;
    return doc_add_block(doc,
                         SOLAR_OS_DOC_BLOCK_PARAGRAPH,
                         0,
                         source_start,
                         source_end,
                         owned,
                         SOLAR_OS_DOC_RUN_PLAIN);
}

static size_t doc_trim_start(const char *source, size_t start, size_t end)
{
    while (start < end && (source[start] == ' ' || source[start] == '\t')) {
        start++;
    }
    return start;
}

static size_t doc_trim_end(const char *source, size_t start, size_t end)
{
    while (end > start && (source[end - 1U] == ' ' || source[end - 1U] == '\t')) {
        end--;
    }
    return end;
}

static size_t doc_text_line_text_len(const char *source, size_t start, size_t end)
{
    const size_t trimmed_start = doc_trim_start(source, start, end);
    const size_t trimmed_end = doc_trim_end(source, trimmed_start, end);
    return trimmed_end > trimmed_start ? trimmed_end - trimmed_start : 0;
}

static bool doc_text_line_is_indented(const char *source, size_t start, size_t end)
{
    return start < end && (source[start] == ' ' || source[start] == '\t');
}

static bool doc_text_line_has_tab(const char *source, size_t start, size_t end)
{
    for (size_t i = start; i < end; i++) {
        if (source[i] == '\t') {
            return true;
        }
    }
    return false;
}

static bool doc_text_line_starts_list(const char *source, size_t start, size_t end)
{
    start = doc_trim_start(source, start, end);
    if (start >= end) {
        return false;
    }

    const char first = source[start];
    if ((first == '-' || first == '*' || first == '+') &&
        start + 1U < end &&
        (source[start + 1U] == ' ' || source[start + 1U] == '\t')) {
        return true;
    }
    if (first == '>' || first == '#') {
        return true;
    }
    if (isdigit((unsigned char)first)) {
        size_t p = start + 1U;
        while (p < end && isdigit((unsigned char)source[p])) {
            p++;
        }
        return p > start &&
            p + 1U < end &&
            (source[p] == '.' || source[p] == ')') &&
            (source[p + 1U] == ' ' || source[p + 1U] == '\t');
    }
    return false;
}

static bool doc_text_lines_joinable(const char *source,
                                    size_t current_start,
                                    size_t current_end,
                                    size_t next_start,
                                    size_t next_end)
{
    if (doc_line_blank(source, current_start, current_end) ||
        doc_line_blank(source, next_start, next_end) ||
        doc_text_line_is_indented(source, current_start, current_end) ||
        doc_text_line_is_indented(source, next_start, next_end) ||
        doc_text_line_has_tab(source, current_start, current_end) ||
        doc_text_line_has_tab(source, next_start, next_end) ||
        doc_text_line_starts_list(source, current_start, current_end) ||
        doc_text_line_starts_list(source, next_start, next_end) ||
        doc_text_line_text_len(source, current_start, current_end) < DOC_TEXT_SOFT_LINE_MIN) {
        return false;
    }
    return true;
}

esp_err_t solar_os_doc_parse_text(solar_os_doc_t *doc,
                                  const char *source,
                                  size_t source_len,
                                  const char *path)
{
    if (doc == NULL || (source == NULL && source_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    doc_clear(doc, true);
    if (path != NULL) {
        strlcpy(doc->path, path, sizeof(doc->path));
    }

    doc->source = doc_malloc(source_len + 1U);
    if (doc->source == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (source_len > 0) {
        memcpy(doc->source, source, source_len);
    }
    doc->source[source_len] = '\0';
    doc->source_len = source_len;

    for (size_t offset = 0; offset < source_len;) {
        const size_t line_start = offset;
        const size_t line_end = doc_line_end(source, source_len, offset);
        const size_t next_line = doc_next_line(source, source_len, offset);

        if (doc_line_blank(source, line_start, line_end)) {
            esp_err_t ret = doc_add_plain_block(doc,
                                                SOLAR_OS_DOC_BLOCK_BLANK,
                                                0,
                                                line_start,
                                                next_line,
                                                doc_copy_text(""),
                                                SOLAR_OS_DOC_RUN_PLAIN);
            if (ret != ESP_OK) {
                return ret;
            }
            offset = next_line;
            continue;
        }

        size_t para_start = line_start;
        size_t para_end = line_end;
        size_t last_line_start = line_start;
        size_t last_line_end = line_end;
        size_t scan = next_line;
        while (scan < source_len) {
            const size_t candidate_start = scan;
            const size_t candidate_end = doc_line_end(source, source_len, scan);
            if (!doc_text_lines_joinable(source,
                                         last_line_start,
                                         last_line_end,
                                         candidate_start,
                                         candidate_end)) {
                break;
            }
            para_end = candidate_end;
            last_line_start = candidate_start;
            last_line_end = candidate_end;
            scan = doc_next_line(source, source_len, scan);
        }

        char *paragraph = NULL;
        size_t paragraph_len = 0;
        size_t paragraph_cap = 0;
        for (size_t line = para_start; line <= last_line_start && line < source_len;) {
            const size_t current_end = doc_line_end(source, source_len, line);
            const size_t text_start = doc_trim_start(source, line, current_end);
            const size_t text_end = doc_trim_end(source, text_start, current_end);
            esp_err_t ret = doc_append_paragraph_text(&paragraph,
                                                      &paragraph_len,
                                                      &paragraph_cap,
                                                      source,
                                                      text_start,
                                                      text_end);
            if (ret != ESP_OK) {
                doc_free(paragraph);
                return ret;
            }
            if (line == last_line_start) {
                break;
            }
            line = doc_next_line(source, source_len, line);
        }

        char *owned = paragraph;
        paragraph = NULL;
        paragraph_len = 0;
        paragraph_cap = 0;
        esp_err_t ret = doc_add_plain_block(doc,
                                            SOLAR_OS_DOC_BLOCK_PARAGRAPH,
                                            0,
                                            para_start,
                                            para_end,
                                            owned,
                                            SOLAR_OS_DOC_RUN_PLAIN);
        if (ret != ESP_OK) {
            doc_free(paragraph);
            return ret;
        }
        offset = doc_next_line(source, source_len, last_line_start);
    }

    if (doc->block_count == 0) {
        return doc_add_plain_block(doc,
                                   SOLAR_OS_DOC_BLOCK_BLANK,
                                   0,
                                   0,
                                   0,
                                   doc_copy_text(""),
                                   SOLAR_OS_DOC_RUN_PLAIN);
    }
    return ESP_OK;
}

esp_err_t solar_os_doc_parse_markdown(solar_os_doc_t *doc,
                                      const char *source,
                                      size_t source_len,
                                      const char *path)
{
    if (doc == NULL || (source == NULL && source_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    doc_clear(doc, true);
    if (path != NULL) {
        strlcpy(doc->path, path, sizeof(doc->path));
    }

    doc->source = doc_malloc(source_len + 1U);
    if (doc->source == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (source_len > 0) {
        memcpy(doc->source, source, source_len);
    }
    doc->source[source_len] = '\0';
    doc->source_len = source_len;

    char *paragraph = NULL;
    size_t paragraph_len = 0;
    size_t paragraph_cap = 0;
    size_t paragraph_start = 0;
    size_t paragraph_end = 0;
    bool in_fence = false;
    char *pre = NULL;
    size_t pre_len = 0;
    size_t pre_cap = 0;
    size_t pre_start = 0;

    for (size_t offset = 0; offset < source_len;) {
        const size_t line_start = offset;
        const size_t line_end = doc_line_end(source, source_len, offset);
        const size_t next_line = doc_next_line(source, source_len, offset);

        if (in_fence) {
            if (doc_parse_fence(source, line_start, line_end)) {
                esp_err_t ret = doc_add_block(doc,
                                             SOLAR_OS_DOC_BLOCK_PRE,
                                             0,
                                             pre_start,
                                             next_line,
                                             pre != NULL ? pre : doc_copy_text(""),
                                             SOLAR_OS_DOC_RUN_CODE);
                if (ret != ESP_OK) {
                    doc_free(pre);
                    return ret;
                }
                pre = NULL;
                pre_len = 0;
                pre_cap = 0;
                in_fence = false;
            } else {
                esp_err_t ret = doc_append_raw_text(&pre,
                                                    &pre_len,
                                                    &pre_cap,
                                                    source,
                                                    line_start,
                                                    line_end,
                                                    true);
                if (ret != ESP_OK) {
                    doc_free(pre);
                    return ret;
                }
            }
            offset = next_line;
            continue;
        }

        if (doc_parse_fence(source, line_start, line_end)) {
            esp_err_t ret = doc_flush_paragraph(doc,
                                                &paragraph,
                                                &paragraph_len,
                                                &paragraph_cap,
                                                paragraph_start,
                                                paragraph_end);
            if (ret != ESP_OK) {
                return ret;
            }
            in_fence = true;
            pre_start = next_line;
            offset = next_line;
            continue;
        }

        if (doc_line_blank(source, line_start, line_end)) {
            esp_err_t ret = doc_flush_paragraph(doc,
                                                &paragraph,
                                                &paragraph_len,
                                                &paragraph_cap,
                                                paragraph_start,
                                                paragraph_end);
            if (ret != ESP_OK) {
                return ret;
            }
            ret = doc_add_block(doc,
                                SOLAR_OS_DOC_BLOCK_BLANK,
                                0,
                                line_start,
                                next_line,
                                doc_copy_text(""),
                                SOLAR_OS_DOC_RUN_PLAIN);
            if (ret != ESP_OK) {
                return ret;
            }
            offset = next_line;
            continue;
        }

        size_t ignored_list_start = 0;
        if (doc_line_is_indented_code(source, line_start, line_end) &&
            !doc_parse_list(source, line_start, line_end, &ignored_list_start)) {
            esp_err_t ret = doc_flush_paragraph(doc,
                                                &paragraph,
                                                &paragraph_len,
                                                &paragraph_cap,
                                                paragraph_start,
                                                paragraph_end);
            if (ret != ESP_OK) {
                return ret;
            }

            char *indented = NULL;
            size_t indented_len = 0;
            size_t indented_cap = 0;
            const size_t code_start = line_start;
            size_t code_end = line_end;
            size_t scan = offset;
            while (scan < source_len) {
                const size_t current_start = scan;
                const size_t current_end = doc_line_end(source, source_len, scan);
                const size_t current_next = doc_next_line(source, source_len, scan);
                if (!doc_line_blank(source, current_start, current_end) &&
                    !doc_line_is_indented_code(source, current_start, current_end)) {
                    break;
                }
                const size_t text_start = doc_line_blank(source, current_start, current_end) ?
                    current_start :
                    doc_indented_code_text_start(source, current_start, current_end);
                ret = doc_append_raw_text(&indented,
                                          &indented_len,
                                          &indented_cap,
                                          source,
                                          text_start,
                                          current_end,
                                          true);
                if (ret != ESP_OK) {
                    doc_free(indented);
                    return ret;
                }
                code_end = current_next;
                scan = current_next;
            }

            ret = doc_add_block(doc,
                                SOLAR_OS_DOC_BLOCK_PRE,
                                0,
                                code_start,
                                code_end,
                                indented != NULL ? indented : doc_copy_text(""),
                                SOLAR_OS_DOC_RUN_CODE);
            if (ret != ESP_OK) {
                return ret;
            }
            offset = scan;
            continue;
        }

        const size_t after_header = next_line;
        const size_t table_delim_end = after_header < source_len ?
            doc_line_end(source, source_len, after_header) :
            after_header;
        if (doc_line_has_table_pipe(source, line_start, line_end) &&
            after_header < source_len &&
            doc_parse_table_delimiter(source, after_header, table_delim_end)) {
            esp_err_t ret = doc_flush_paragraph(doc,
                                                &paragraph,
                                                &paragraph_len,
                                                &paragraph_cap,
                                                paragraph_start,
                                                paragraph_end);
            if (ret != ESP_OK) {
                return ret;
            }

            ret = doc_add_block(doc,
                                SOLAR_OS_DOC_BLOCK_TABLE_ROW,
                                1,
                                line_start,
                                next_line,
                                doc_copy_table_row(source, line_start, line_end),
                                SOLAR_OS_DOC_RUN_BOLD);
            if (ret != ESP_OK) {
                return ret;
            }

            size_t scan = doc_next_line(source, source_len, after_header);
            while (scan < source_len) {
                const size_t row_start = scan;
                const size_t row_end = doc_line_end(source, source_len, scan);
                const size_t row_next = doc_next_line(source, source_len, scan);
                if (doc_line_blank(source, row_start, row_end) ||
                    !doc_line_has_table_pipe(source, row_start, row_end)) {
                    break;
                }
                ret = doc_add_block(doc,
                                    SOLAR_OS_DOC_BLOCK_TABLE_ROW,
                                    0,
                                    row_start,
                                    row_next,
                                    doc_copy_table_row(source, row_start, row_end),
                                    SOLAR_OS_DOC_RUN_PLAIN);
                if (ret != ESP_OK) {
                    return ret;
                }
                scan = row_next;
            }
            offset = scan;
            continue;
        }

        size_t image_target_start = 0;
        size_t image_target_end = 0;
        if (doc_parse_image_line(source,
                                 line_start,
                                 line_end,
                                 &image_target_start,
                                 &image_target_end)) {
            esp_err_t ret = doc_flush_paragraph(doc,
                                                &paragraph,
                                                &paragraph_len,
                                                &paragraph_cap,
                                                paragraph_start,
                                                paragraph_end);
            if (ret != ESP_OK) {
                return ret;
            }
            ret = doc_add_image_block(doc,
                                      line_start,
                                      next_line,
                                      doc_copy_range(source, image_target_start, image_target_end));
            if (ret != ESP_OK) {
                return ret;
            }
            offset = next_line;
            continue;
        }

        if (doc_parse_rule(source, line_start, line_end)) {
            esp_err_t ret = doc_flush_paragraph(doc,
                                                &paragraph,
                                                &paragraph_len,
                                                &paragraph_cap,
                                                paragraph_start,
                                                paragraph_end);
            if (ret != ESP_OK) {
                return ret;
            }
            ret = doc_add_empty_block(doc,
                                      SOLAR_OS_DOC_BLOCK_RULE,
                                      0,
                                      line_start,
                                      next_line);
            if (ret != ESP_OK) {
                return ret;
            }
            offset = next_line;
            continue;
        }

        uint8_t heading_level = 0;
        size_t text_start = 0;
        if (doc_parse_heading(source, line_start, line_end, &heading_level, &text_start)) {
            esp_err_t ret = doc_flush_paragraph(doc,
                                                &paragraph,
                                                &paragraph_len,
                                                &paragraph_cap,
                                                paragraph_start,
                                                paragraph_end);
            if (ret != ESP_OK) {
                return ret;
            }
            ret = doc_add_block(doc,
                                SOLAR_OS_DOC_BLOCK_HEADING,
                                heading_level,
                                text_start,
                                next_line,
                                doc_copy_range(source, text_start, line_end),
                                SOLAR_OS_DOC_RUN_BOLD);
            if (ret != ESP_OK) {
                return ret;
            }
        } else if (doc_parse_list(source, line_start, line_end, &text_start)) {
            esp_err_t ret = doc_flush_paragraph(doc,
                                                &paragraph,
                                                &paragraph_len,
                                                &paragraph_cap,
                                                paragraph_start,
                                                paragraph_end);
            if (ret != ESP_OK) {
                return ret;
            }
            ret = doc_add_block(doc,
                                SOLAR_OS_DOC_BLOCK_LIST_ITEM,
                                1,
                                text_start,
                                next_line,
                                doc_copy_range(source, text_start, line_end),
                                SOLAR_OS_DOC_RUN_PLAIN);
            if (ret != ESP_OK) {
                return ret;
            }
        } else if (doc_parse_quote(source, line_start, line_end, &text_start)) {
            esp_err_t ret = doc_flush_paragraph(doc,
                                                &paragraph,
                                                &paragraph_len,
                                                &paragraph_cap,
                                                paragraph_start,
                                                paragraph_end);
            if (ret != ESP_OK) {
                return ret;
            }
            ret = doc_add_block(doc,
                                SOLAR_OS_DOC_BLOCK_QUOTE,
                                1,
                                text_start,
                                next_line,
                                doc_copy_range(source, text_start, line_end),
                                SOLAR_OS_DOC_RUN_ITALIC);
            if (ret != ESP_OK) {
                return ret;
            }
        } else {
            if (paragraph_len == 0) {
                paragraph_start = line_start;
            }
            paragraph_end = next_line;
            esp_err_t ret = doc_append_paragraph_text(&paragraph,
                                                      &paragraph_len,
                                                      &paragraph_cap,
                                                      source,
                                                      line_start,
                                                      line_end);
            if (ret != ESP_OK) {
                doc_free(paragraph);
                return ret;
            }
        }
        offset = next_line;
    }

    if (in_fence) {
        esp_err_t ret = doc_add_block(doc,
                                      SOLAR_OS_DOC_BLOCK_PRE,
                                      0,
                                      pre_start,
                                      source_len,
                                      pre != NULL ? pre : doc_copy_text(""),
                                      SOLAR_OS_DOC_RUN_CODE);
        if (ret != ESP_OK) {
            doc_free(pre);
            return ret;
        }
        pre = NULL;
    }

    esp_err_t ret = doc_flush_paragraph(doc,
                                        &paragraph,
                                        &paragraph_len,
                                        &paragraph_cap,
                                        paragraph_start,
                                        paragraph_end);
    if (ret != ESP_OK) {
        doc_free(paragraph);
        return ret;
    }

    if (doc->block_count == 0) {
        return doc_add_block(doc,
                             SOLAR_OS_DOC_BLOCK_BLANK,
                             0,
                             0,
                             0,
                             doc_copy_text(""),
                             SOLAR_OS_DOC_RUN_PLAIN);
    }
    return ESP_OK;
}

esp_err_t solar_os_doc_load_path_as(solar_os_doc_t *doc, const char *path, bool markdown)
{
    if (doc == NULL || path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > DOC_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    const size_t len = (size_t)st.st_size;
    char *data = doc_malloc(len + 1U);
    if (data == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    const size_t read_len = len > 0 ? fread(data, 1, len, file) : 0;
    const bool failed = ferror(file) || read_len != len;
    fclose(file);
    if (failed) {
        doc_free(data);
        return ESP_FAIL;
    }
    data[len] = '\0';

    const esp_err_t ret = markdown ?
        solar_os_doc_parse_markdown(doc, data, len, path) :
        solar_os_doc_parse_text(doc, data, len, path);
    doc_free(data);
    return ret;
}

esp_err_t solar_os_doc_load_path(solar_os_doc_t *doc, const char *path)
{
    return solar_os_doc_load_path_as(doc, path, true);
}

esp_err_t solar_os_doc_write_markdown(const solar_os_doc_t *doc, const char *path)
{
    if (doc == NULL || path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    bool ok = true;
    for (size_t i = 0; ok && i < doc->block_count; i++) {
        const solar_os_doc_block_t *block = &doc->blocks[i];
        const char *text = block->text != NULL ? block->text : "";
        switch (block->type) {
        case SOLAR_OS_DOC_BLOCK_HEADING:
            for (uint8_t h = 0; h < block->level && h < 6U; h++) {
                ok = fputc('#', file) != EOF;
            }
            ok = ok && fprintf(file, " %s\n", text) >= 0;
            break;
        case SOLAR_OS_DOC_BLOCK_LIST_ITEM:
            ok = fprintf(file, "- %s\n", text) >= 0;
            break;
        case SOLAR_OS_DOC_BLOCK_QUOTE:
            ok = fprintf(file, "> %s\n", text) >= 0;
            break;
        case SOLAR_OS_DOC_BLOCK_PRE:
            ok = fprintf(file, "```\n%s\n```\n", text) >= 0;
            break;
        case SOLAR_OS_DOC_BLOCK_RULE:
            ok = fprintf(file, "---\n") >= 0;
            break;
        case SOLAR_OS_DOC_BLOCK_TABLE_ROW:
            ok = fprintf(file, "| %s |\n", text) >= 0;
            break;
        case SOLAR_OS_DOC_BLOCK_IMAGE:
            ok = fprintf(file, "![](%s)\n", text) >= 0;
            break;
        case SOLAR_OS_DOC_BLOCK_BLANK:
            ok = fputc('\n', file) != EOF;
            break;
        case SOLAR_OS_DOC_BLOCK_PARAGRAPH:
        default:
            ok = fprintf(file, "%s\n", text) >= 0;
            break;
        }
    }

    if (fclose(file) != 0) {
        ok = false;
    }
    return ok ? ESP_OK : ESP_FAIL;
}

static doc_style_t doc_base_style_for(int zoom)
{
    if (zoom < 0) {
        zoom = 0;
    }
    if (zoom > 4) {
        zoom = 4;
    }

    static const doc_style_t styles[] = {
        {.font = SOLAR_OS_GFX_FONT_MONO_12, .char_w = 6, .line_h = 14, .baseline = 10, .paragraph_gap = 4, .heading_gap = 8, .block_gap = 2, .pixel_size = 12, .face = 0},
        {.font = SOLAR_OS_GFX_FONT_MONO_14, .char_w = 7, .line_h = 16, .baseline = 12, .paragraph_gap = 5, .heading_gap = 9, .block_gap = 3, .pixel_size = 14, .face = 0},
        {.font = SOLAR_OS_GFX_FONT_MONO_16, .char_w = 8, .line_h = 18, .baseline = 14, .paragraph_gap = 7, .heading_gap = 12, .block_gap = 4, .pixel_size = 16, .face = 0},
        {.font = SOLAR_OS_GFX_FONT_MONO_18, .char_w = 9, .line_h = 20, .baseline = 15, .paragraph_gap = 9, .heading_gap = 15, .block_gap = 5, .pixel_size = 18, .face = 0},
        {.font = SOLAR_OS_GFX_FONT_MONO_20, .char_w = 10, .line_h = 22, .baseline = 17, .paragraph_gap = 11, .heading_gap = 18, .block_gap = 6, .pixel_size = 20, .face = 0},
    };
    doc_style_t style = styles[zoom];
    return style;
}

static solar_os_gfx_font_t doc_font_for_style(int zoom, uint8_t style)
{
    if (zoom < 0) {
        zoom = 0;
    }
    if (zoom > 4) {
        zoom = 4;
    }

    static const solar_os_gfx_font_t regular[] = {
        SOLAR_OS_GFX_FONT_MONO_12,
        SOLAR_OS_GFX_FONT_MONO_14,
        SOLAR_OS_GFX_FONT_MONO_16,
        SOLAR_OS_GFX_FONT_MONO_18,
        SOLAR_OS_GFX_FONT_MONO_20,
    };
    static const solar_os_gfx_font_t bold[] = {
        SOLAR_OS_GFX_FONT_BOLD_12,
        SOLAR_OS_GFX_FONT_BOLD_14,
        SOLAR_OS_GFX_FONT_BOLD_16,
        SOLAR_OS_GFX_FONT_BOLD_18,
        SOLAR_OS_GFX_FONT_BOLD_20,
    };
    static const solar_os_gfx_font_t italic[] = {
        SOLAR_OS_GFX_FONT_ITALIC_12,
        SOLAR_OS_GFX_FONT_ITALIC_14,
        SOLAR_OS_GFX_FONT_ITALIC_16,
        SOLAR_OS_GFX_FONT_ITALIC_18,
        SOLAR_OS_GFX_FONT_ITALIC_20,
    };
    static const solar_os_gfx_font_t bold_italic[] = {
        SOLAR_OS_GFX_FONT_BOLD_ITALIC_12,
        SOLAR_OS_GFX_FONT_BOLD_ITALIC_14,
        SOLAR_OS_GFX_FONT_BOLD_ITALIC_16,
        SOLAR_OS_GFX_FONT_BOLD_ITALIC_18,
        SOLAR_OS_GFX_FONT_BOLD_ITALIC_20,
    };

    const bool want_bold = (style & SOLAR_OS_DOC_RUN_BOLD) != 0;
    const bool want_italic = (style & SOLAR_OS_DOC_RUN_ITALIC) != 0;
    if (want_bold && want_italic) {
        return bold_italic[zoom];
    }
    if (want_bold) {
        return bold[zoom];
    }
    if (want_italic) {
        return italic[zoom];
    }
    return regular[zoom];
}

static doc_style_t doc_style_for(int zoom, uint8_t style)
{
    doc_style_t result = doc_base_style_for(zoom);
    result.font = doc_font_for_style(zoom, style);
    result.face = 0;
    return result;
}

void solar_os_doc_layout_init(solar_os_doc_layout_t *layout)
{
    if (layout != NULL) {
        memset(layout, 0, sizeof(*layout));
    }
}

void solar_os_doc_layout_free(solar_os_doc_layout_t *layout)
{
    if (layout == NULL) {
        return;
    }

    doc_free(layout->lines);
    doc_free(layout->runs);
    memset(layout, 0, sizeof(*layout));
}

static esp_err_t doc_layout_reserve_lines(solar_os_doc_layout_t *layout, size_t need)
{
    if (layout->line_capacity >= need) {
        return ESP_OK;
    }

    size_t next = layout->line_capacity > 0 ? layout->line_capacity * 2U : 64U;
    while (next < need) {
        next *= 2U;
    }
    const size_t old_capacity = layout->line_capacity;
    solar_os_doc_layout_line_t *lines = doc_realloc(layout->lines, next * sizeof(lines[0]));
    if (lines == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (next > old_capacity) {
        memset(&lines[old_capacity], 0, (next - old_capacity) * sizeof(lines[0]));
    }
    layout->lines = lines;
    layout->line_capacity = next;
    return ESP_OK;
}

static esp_err_t doc_layout_reserve_runs(solar_os_doc_layout_t *layout, size_t need)
{
    if (layout->run_capacity >= need) {
        return ESP_OK;
    }

    size_t next = layout->run_capacity > 0 ? layout->run_capacity * 2U : 128U;
    while (next < need) {
        next *= 2U;
    }
    const size_t old_capacity = layout->run_capacity;
    solar_os_doc_layout_run_t *runs = doc_realloc(layout->runs, next * sizeof(runs[0]));
    if (runs == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (next > old_capacity) {
        memset(&runs[old_capacity], 0, (next - old_capacity) * sizeof(runs[0]));
    }
    layout->runs = runs;
    layout->run_capacity = next;
    return ESP_OK;
}

static esp_err_t doc_layout_begin_line(solar_os_doc_layout_t *layout,
                                       size_t block_index,
                                       int x,
                                       int y,
                                       const doc_style_t *style,
                                       size_t *line_index)
{
    esp_err_t ret = doc_layout_reserve_lines(layout, layout->line_count + 1U);
    if (ret != ESP_OK) {
        return ret;
    }

    const size_t index = layout->line_count++;
    layout->lines[index] = (solar_os_doc_layout_line_t){
        .x = x,
        .y = y,
        .width = 0,
        .height = style->line_h,
        .baseline = style->baseline,
        .block_index = block_index,
        .run_start = layout->run_count,
        .run_count = 0,
        .source_start = SIZE_MAX,
        .source_end = 0,
    };
    if (line_index != NULL) {
        *line_index = index;
    }
    return ESP_OK;
}

static void doc_layout_update_line(solar_os_doc_layout_line_t *line,
                                   const solar_os_doc_layout_run_t *run)
{
    if (line == NULL || run == NULL) {
        return;
    }

    const int right = run->x + run->width;
    if (right > line->x + line->width) {
        line->width = right - line->x;
    }
    if (run->height > line->height) {
        line->height = run->height;
    }
    if (run->baseline > line->baseline) {
        line->baseline = run->baseline;
    }
    if (run->source_start != SIZE_MAX && run->source_start < line->source_start) {
        line->source_start = run->source_start;
    }
    if (run->source_end > line->source_end) {
        line->source_end = run->source_end;
    }
    line->run_count++;
}

static esp_err_t doc_layout_add_run(solar_os_doc_layout_t *layout,
                                    size_t line_index,
                                    size_t block_index,
                                    size_t run_index,
                                    size_t text_offset,
                                    size_t text_len,
                                    size_t source_start,
                                    size_t source_end,
                                    uint8_t style_bits,
                                    int style_zoom,
                                    int x,
                                    int y,
                                    const char *literal)
{
    esp_err_t ret = doc_layout_reserve_runs(layout, layout->run_count + 1U);
    if (ret != ESP_OK) {
        return ret;
    }

    const doc_style_t style = doc_style_for(style_zoom, style_bits);
    solar_os_doc_layout_run_t *run = &layout->runs[layout->run_count++];
    *run = (solar_os_doc_layout_run_t){
        .x = x,
        .y = y,
        .width = (int)text_len * style.char_w,
        .height = style.line_h,
        .baseline = style.baseline,
        .block_index = block_index,
        .run_index = run_index,
        .text_offset = text_offset,
        .text_len = text_len,
        .source_start = source_start,
        .source_end = source_end,
        .style = style_bits,
        .font = style.font,
        .char_w = style.char_w,
        .pixel_size = style.pixel_size,
        .face = style.face,
    };
    if (literal != NULL) {
        strlcpy(run->literal, literal, sizeof(run->literal));
    }

    doc_layout_update_line(&layout->lines[line_index], run);
    return ESP_OK;
}

static const solar_os_doc_run_t *doc_block_run_at(const solar_os_doc_t *doc,
                                                  const solar_os_doc_block_t *block,
                                                  size_t offset,
                                                  size_t *run_index)
{
    if (doc == NULL || block == NULL || block->run_count == 0) {
        return NULL;
    }

    for (size_t i = 0; i < block->run_count; i++) {
        const size_t index = block->run_start + i;
        const solar_os_doc_run_t *run = &doc->runs[index];
        if (offset >= run->text_offset && offset < run->text_offset + run->text_len) {
            if (run_index != NULL) {
                *run_index = index;
            }
            return run;
        }
    }

    const size_t index = block->run_start + block->run_count - 1U;
    if (run_index != NULL) {
        *run_index = index;
    }
    return &doc->runs[index];
}

static size_t doc_next_word(const char *text, size_t len, size_t offset, size_t *word_end)
{
    while (offset < len && isspace((unsigned char)text[offset])) {
        offset++;
    }
    size_t end = offset;
    while (end < len && !isspace((unsigned char)text[end])) {
        end++;
    }
    if (word_end != NULL) {
        *word_end = end;
    }
    return offset;
}

static esp_err_t doc_layout_add_text_range(solar_os_doc_layout_t *layout,
                                           const solar_os_doc_t *doc,
                                           const solar_os_doc_block_t *block,
                                           size_t block_index,
                                           size_t line_index,
                                           size_t text_start,
                                           size_t text_end,
                                           int *x,
                                           int y,
                                           int style_zoom,
                                           uint8_t extra_style)
{
    size_t offset = text_start;
    while (offset < text_end) {
        size_t run_index = 0;
        const solar_os_doc_run_t *run = doc_block_run_at(doc, block, offset, &run_index);
        size_t run_end = run != NULL ?
            run->text_offset + run->text_len :
            text_end;
        if (run_end <= offset) {
            run = NULL;
            run_end = text_end;
        }
        const size_t end = run_end < text_end ? run_end : text_end;
        const uint8_t style = (run != NULL ? run->style : SOLAR_OS_DOC_RUN_PLAIN) | extra_style;
        const doc_style_t metrics = doc_style_for(style_zoom, style);
        const size_t source_start = run != NULL ?
            run->source_start + (offset - run->text_offset) :
            block->source_start + offset;
        const size_t source_end = run != NULL ?
            run->source_start + (end - run->text_offset) :
            block->source_start + end;

        esp_err_t ret = doc_layout_add_run(layout,
                                           line_index,
                                           block_index,
                                           run_index,
                                           offset,
                                           end - offset,
                                           source_start,
                                           source_end,
                                           style,
                                           style_zoom,
                                           *x,
                                           y,
                                           NULL);
        if (ret != ESP_OK) {
            return ret;
        }
        *x += (int)(end - offset) * metrics.char_w;
        offset = end;
    }
    return ESP_OK;
}

static esp_err_t doc_layout_wrapped_block(solar_os_doc_layout_t *layout,
                                          const solar_os_doc_t *doc,
                                          size_t block_index,
                                          int x,
                                          int *y,
                                          int width,
                                          const char *prefix,
                                          int style_zoom,
                                          uint8_t extra_style)
{
    const solar_os_doc_block_t *block = &doc->blocks[block_index];
    const char *text = block->text != NULL ? block->text : "";
    const size_t len = strlen(text);
    const doc_style_t base = doc_style_for(style_zoom, extra_style);
    const size_t prefix_len = prefix != NULL ? strlen(prefix) : 0;
    const int prefix_width = (int)prefix_len * base.char_w;
    const int right = x + width;
    size_t line_index = 0;
    int current_x = x;
    int text_x = x;
    size_t line_text_start = 0;
    size_t line_text_end = 0;
    bool line_has_text = false;

    #define DOC_LAYOUT_BEGIN_WRAPPED_LINE()                                              \
        do {                                                                              \
            ret = doc_layout_begin_line(layout, block_index, x, *y, &base, &line_index);  \
            if (ret != ESP_OK) {                                                         \
                return ret;                                                               \
            }                                                                             \
            current_x = x;                                                                \
            text_x = x + prefix_width;                                                    \
            line_has_text = false;                                                        \
            if (prefix_len > 0) {                                                         \
                ret = doc_layout_add_run(layout,                                          \
                                         line_index,                                      \
                                         block_index,                                     \
                                         SIZE_MAX,                                        \
                                         SIZE_MAX,                                        \
                                         prefix_len,                                      \
                                         block->source_start,                             \
                                         block->source_start,                             \
                                         extra_style,                                     \
                                         style_zoom,                                      \
                                         current_x,                                       \
                                         *y,                                              \
                                         prefix);                                         \
                if (ret != ESP_OK) {                                                     \
                    return ret;                                                           \
                }                                                                         \
                current_x += prefix_width;                                                \
            }                                                                             \
        } while (0)

    #define DOC_LAYOUT_FLUSH_WRAPPED_TEXT()                                               \
        do {                                                                              \
            if (line_has_text) {                                                          \
                ret = doc_layout_add_text_range(layout,                                   \
                                                doc,                                      \
                                                block,                                    \
                                                block_index,                              \
                                                line_index,                               \
                                                line_text_start,                          \
                                                line_text_end,                            \
                                                &text_x,                                  \
                                                *y,                                       \
                                                style_zoom,                               \
                                                extra_style);                             \
                if (ret != ESP_OK) {                                                     \
                    return ret;                                                           \
                }                                                                         \
                line_has_text = false;                                                    \
            }                                                                             \
        } while (0)

    esp_err_t ret = ESP_OK;
    DOC_LAYOUT_BEGIN_WRAPPED_LINE();

    size_t offset = 0;
    while (offset < len) {
        size_t word_end = 0;
        size_t word = doc_next_word(text, len, offset, &word_end);
        if (word >= len) {
            break;
        }

        while (word < word_end) {
            const size_t remaining_len = word_end - word;
            const int word_width = (int)remaining_len * base.char_w;
            const size_t gap_chars = line_has_text && word > line_text_end ?
                word - line_text_end :
                (line_has_text ? 1U : 0U);
            const int space_width = (int)gap_chars * base.char_w;

            if (line_has_text && current_x + space_width + word_width > right) {
                DOC_LAYOUT_FLUSH_WRAPPED_TEXT();
                *y += layout->lines[line_index].height;
                DOC_LAYOUT_BEGIN_WRAPPED_LINE();
                continue;
            }

            if (line_has_text) {
                current_x += space_width;
                line_text_end = word_end;
                current_x += word_width;
                word = word_end;
                continue;
            }

            int fit_width = right - current_x;
            if (fit_width < base.char_w) {
                fit_width = base.char_w;
            }
            size_t fit_chars = (size_t)(fit_width / base.char_w);
            if (fit_chars == 0) {
                fit_chars = 1;
            }
            if (fit_chars > remaining_len) {
                fit_chars = remaining_len;
            }

            line_text_start = word;
            line_text_end = word + fit_chars;
            current_x += (int)fit_chars * base.char_w;
            line_has_text = true;
            word += fit_chars;

            if (word < word_end) {
                DOC_LAYOUT_FLUSH_WRAPPED_TEXT();
                *y += layout->lines[line_index].height;
                DOC_LAYOUT_BEGIN_WRAPPED_LINE();
            }
        }

        offset = word_end;
    }

    while (offset < len) {
        const size_t remaining = len - offset;
        int fit_width = right - current_x;
        if (fit_width < base.char_w) {
            if (line_has_text) {
                DOC_LAYOUT_FLUSH_WRAPPED_TEXT();
                *y += layout->lines[line_index].height;
                DOC_LAYOUT_BEGIN_WRAPPED_LINE();
                continue;
            }
            fit_width = base.char_w;
        }

        size_t fit_chars = (size_t)(fit_width / base.char_w);
        if (fit_chars == 0) {
            fit_chars = 1;
        }
        if (fit_chars > remaining) {
            fit_chars = remaining;
        }

        if (!line_has_text) {
            line_text_start = offset;
        }
        line_text_end = offset + fit_chars;
        current_x += (int)fit_chars * base.char_w;
        line_has_text = true;
        offset += fit_chars;
    }

    DOC_LAYOUT_FLUSH_WRAPPED_TEXT();
    if (layout->lines[line_index].run_count == 0 && prefix_len == 0) {
        ret = doc_layout_add_run(layout,
                                 line_index,
                                 block_index,
                                 block->run_start,
                                 0,
                                 0,
                                 block->source_start,
                                 block->source_start,
                                 extra_style,
                                 style_zoom,
                                 current_x,
                                 *y,
                                 NULL);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    *y += layout->lines[line_index].height;

    #undef DOC_LAYOUT_FLUSH_WRAPPED_TEXT
    #undef DOC_LAYOUT_BEGIN_WRAPPED_LINE
    return ESP_OK;
}

static esp_err_t doc_layout_pre_block(solar_os_doc_layout_t *layout,
                                      const solar_os_doc_t *doc,
                                      size_t block_index,
                                      int x,
                                      int *y,
                                      int width)
{
    const solar_os_doc_block_t *block = &doc->blocks[block_index];
    const char *text = block->text != NULL ? block->text : "";
    const size_t len = strlen(text);
    const doc_style_t style = doc_style_for(layout->zoom, SOLAR_OS_DOC_RUN_CODE);
    int max_chars_i = width / style.char_w;
    if (max_chars_i < 1) {
        max_chars_i = 1;
    }
    const size_t max_chars = (size_t)max_chars_i;
    size_t start = 0;

    while (start < len || (len == 0 && start == 0)) {
        size_t end = start;
        while (end < len && text[end] != '\n' && text[end] != '\r') {
            end++;
        }

        size_t chunk = start;
        bool emitted = false;
        while (chunk < end || !emitted) {
            const size_t remaining = end > chunk ? end - chunk : 0;
            const size_t chunk_len = remaining > max_chars ? max_chars : remaining;
            size_t line_index = 0;
            esp_err_t ret = doc_layout_begin_line(layout, block_index, x, *y, &style, &line_index);
            if (ret != ESP_OK) {
                return ret;
            }
            ret = doc_layout_add_run(layout,
                                     line_index,
                                     block_index,
                                     block->run_start,
                                     chunk,
                                     chunk_len,
                                     block->source_start + chunk,
                                     block->source_start + chunk + chunk_len,
                                     SOLAR_OS_DOC_RUN_CODE,
                                     layout->zoom,
                                     x,
                                     *y,
                                     NULL);
            if (ret != ESP_OK) {
                return ret;
            }

            *y += layout->lines[line_index].height;
            emitted = true;
            if (chunk_len == 0) {
                break;
            }
            chunk += chunk_len;
        }

        if (end >= len) {
            break;
        }
        if (text[end] == '\r' && end + 1U < len && text[end + 1U] == '\n') {
            start = end + 2U;
        } else {
            start = end + 1U;
        }
    }
    return ESP_OK;
}

static esp_err_t doc_layout_blank_block(solar_os_doc_layout_t *layout,
                                        const solar_os_doc_t *doc,
                                        size_t block_index,
                                        int x,
                                        int *y)
{
    const solar_os_doc_block_t *block = &doc->blocks[block_index];
    const doc_style_t style = doc_base_style_for(layout->zoom);
    size_t line_index = 0;
    esp_err_t ret = doc_layout_begin_line(layout, block_index, x, *y, &style, &line_index);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = doc_layout_add_run(layout,
                             line_index,
                             block_index,
                             block->run_start,
                             0,
                             0,
                             block->source_start,
                             block->source_end,
                             SOLAR_OS_DOC_RUN_PLAIN,
                             layout->zoom,
                             x,
                             *y,
                             NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    *y += layout->lines[line_index].height;
    return ESP_OK;
}

static int doc_heading_zoom(int zoom, uint8_t level)
{
    int delta = 0;
    if (level <= 1U) {
        delta = 3;
    } else if (level == 2U) {
        delta = 2;
    } else if (level == 3U) {
        delta = 1;
    }

    zoom += delta;
    if (zoom > 4) {
        zoom = 4;
    }
    if (zoom < 0) {
        zoom = 0;
    }
    return zoom;
}

static esp_err_t doc_layout_rule_block(solar_os_doc_layout_t *layout,
                                       const solar_os_doc_t *doc,
                                       size_t block_index,
                                       int x,
                                       int *y,
                                       int width)
{
    const solar_os_doc_block_t *block = &doc->blocks[block_index];
    doc_style_t style = doc_base_style_for(layout->zoom);
    style.line_h = style.block_gap + 5;
    style.baseline = style.line_h / 2;
    size_t line_index = 0;
    esp_err_t ret = doc_layout_begin_line(layout, block_index, x, *y, &style, &line_index);
    if (ret != ESP_OK) {
        return ret;
    }
    layout->lines[line_index].width = width;
    ret = doc_layout_add_run(layout,
                             line_index,
                             block_index,
                             block->run_start,
                             0,
                             0,
                             block->source_start,
                             block->source_end,
                             SOLAR_OS_DOC_RUN_PLAIN,
                             layout->zoom,
                             x,
                             *y,
                             NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    layout->lines[line_index].width = width;
    *y += layout->lines[line_index].height;
    return ESP_OK;
}

static size_t doc_table_cell_count(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return 1U;
    }
    size_t count = 1U;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '|') {
            count++;
        }
    }
    return count;
}

static esp_err_t doc_layout_table_row(solar_os_doc_layout_t *layout,
                                      const solar_os_doc_t *doc,
                                      size_t block_index,
                                      int x,
                                      int *y,
                                      int width)
{
    const solar_os_doc_block_t *block = &doc->blocks[block_index];
    const char *text = block->text != NULL ? block->text : "";
    const size_t len = strlen(text);
    const uint8_t row_style = block->level != 0 ? SOLAR_OS_DOC_RUN_BOLD : SOLAR_OS_DOC_RUN_PLAIN;
    const doc_style_t style = doc_style_for(layout->zoom, row_style);
    const size_t cells = doc_table_cell_count(text);
    const int row_h = style.line_h + 6;
    size_t line_index = 0;
    esp_err_t ret = doc_layout_begin_line(layout, block_index, x, *y, &style, &line_index);
    if (ret != ESP_OK) {
        return ret;
    }
    layout->lines[line_index].height = row_h;
    layout->lines[line_index].baseline = style.baseline + 3;
    layout->lines[line_index].width = width;

    size_t cell_start = 0;
    for (size_t cell = 0; cell < cells; cell++) {
        size_t cell_end = cell_start;
        while (cell_end < len && text[cell_end] != '|') {
            cell_end++;
        }

        size_t trim_start = cell_start;
        while (trim_start < cell_end && isspace((unsigned char)text[trim_start])) {
            trim_start++;
        }
        size_t trim_end = cell_end;
        while (trim_end > trim_start && isspace((unsigned char)text[trim_end - 1U])) {
            trim_end--;
        }

        const int cell_x = x + (int)(((int64_t)width * (int64_t)cell) / (int64_t)cells);
        const int next_x = x + (int)(((int64_t)width * (int64_t)(cell + 1U)) / (int64_t)cells);
        int available = (next_x - cell_x) - (DOC_TABLE_CELL_PAD * 2);
        if (available < style.char_w) {
            available = style.char_w;
        }
        size_t draw_len = trim_end > trim_start ? trim_end - trim_start : 0;
        const size_t max_chars = (size_t)(available / style.char_w);
        if (draw_len > max_chars) {
            draw_len = max_chars;
        }
        if (draw_len > 0) {
            int text_x = cell_x + DOC_TABLE_CELL_PAD;
            ret = doc_layout_add_text_range(layout,
                                            doc,
                                            block,
                                            block_index,
                                            line_index,
                                            trim_start,
                                            trim_start + draw_len,
                                            &text_x,
                                            *y + 3,
                                            layout->zoom,
                                            row_style);
            if (ret != ESP_OK) {
                return ret;
            }
        }

        cell_start = cell_end < len ? cell_end + 1U : len;
    }

    if (layout->lines[line_index].run_count == 0) {
        ret = doc_layout_add_run(layout,
                                 line_index,
                                 block_index,
                                 block->run_start,
                                 0,
                                 0,
                                 block->source_start,
                                 block->source_end,
                                 row_style,
                                 layout->zoom,
                                 x,
                                 *y,
                                 NULL);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    layout->lines[line_index].height = row_h;
    layout->lines[line_index].width = width;
    *y += row_h;
    return ESP_OK;
}

static esp_err_t doc_layout_image_block(solar_os_doc_layout_t *layout,
                                        const solar_os_doc_t *doc,
                                        size_t block_index,
                                        int x,
                                        int *y,
                                        int width)
{
    const solar_os_doc_block_t *block = &doc->blocks[block_index];
    const doc_style_t style = doc_base_style_for(layout->zoom);
    int draw_w = width;
    int draw_h = style.line_h + 18;
    if (block->image.gray != NULL && block->image.width > 0 && block->image.height > 0) {
        draw_w = (int)block->image.width;
        draw_h = (int)block->image.height;
        if (draw_w > width) {
            draw_h = (int)(((int64_t)draw_h * (int64_t)width) / (int64_t)draw_w);
            draw_w = width;
        }
        if (draw_w < 1) {
            draw_w = 1;
        }
        if (draw_h < 1) {
            draw_h = 1;
        }
    }

    doc_style_t line_style = style;
    line_style.line_h = draw_h + 8;
    line_style.baseline = style.baseline + 4;
    size_t line_index = 0;
    esp_err_t ret = doc_layout_begin_line(layout, block_index, x, *y, &line_style, &line_index);
    if (ret != ESP_OK) {
        return ret;
    }
    layout->lines[line_index].height = line_style.line_h;
    layout->lines[line_index].width = draw_w;
    ret = doc_layout_add_run(layout,
                             line_index,
                             block_index,
                             block->run_start,
                             0,
                             0,
                             block->source_start,
                             block->source_end,
                             SOLAR_OS_DOC_RUN_PLAIN,
                             layout->zoom,
                             x,
                             *y,
                             NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    layout->lines[line_index].width = draw_w;
    *y += line_style.line_h;
    return ESP_OK;
}

esp_err_t solar_os_doc_layout_build(solar_os_doc_layout_t *layout,
                                    const solar_os_doc_t *doc,
                                    int width,
                                    int zoom)
{
    if (layout == NULL || doc == NULL || width <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    layout->line_count = 0;
    layout->run_count = 0;
    layout->width = width;
    layout->height = 0;
    layout->zoom = zoom < 0 ? 0 : (zoom > 4 ? 4 : zoom);

    const doc_style_t base = doc_base_style_for(layout->zoom);
    int y = 0;

    for (size_t i = 0; i < doc->block_count; i++) {
        const solar_os_doc_block_t *block = &doc->blocks[i];
        esp_err_t ret = ESP_OK;

        switch (block->type) {
        case SOLAR_OS_DOC_BLOCK_HEADING:
            y += base.heading_gap / 2;
            ret = doc_layout_wrapped_block(layout,
                                           doc,
                                           i,
                                           0,
                                           &y,
                                           width,
                                           NULL,
                                           doc_heading_zoom(layout->zoom, block->level),
                                           SOLAR_OS_DOC_RUN_BOLD);
            y += base.heading_gap;
            break;
        case SOLAR_OS_DOC_BLOCK_LIST_ITEM:
            ret = doc_layout_wrapped_block(layout, doc, i, 0, &y, width, "   ", layout->zoom, SOLAR_OS_DOC_RUN_PLAIN);
            y += base.block_gap;
            break;
        case SOLAR_OS_DOC_BLOCK_QUOTE:
            ret = doc_layout_wrapped_block(layout, doc, i, 0, &y, width, "   ", layout->zoom, SOLAR_OS_DOC_RUN_ITALIC);
            y += base.paragraph_gap;
            break;
        case SOLAR_OS_DOC_BLOCK_PRE:
            ret = doc_layout_pre_block(layout, doc, i, 4, &y, width - 8 > 8 ? width - 8 : width);
            y += base.paragraph_gap;
            break;
        case SOLAR_OS_DOC_BLOCK_RULE:
            ret = doc_layout_rule_block(layout, doc, i, 0, &y, width);
            y += base.block_gap;
            break;
        case SOLAR_OS_DOC_BLOCK_TABLE_ROW:
            ret = doc_layout_table_row(layout, doc, i, 0, &y, width);
            break;
        case SOLAR_OS_DOC_BLOCK_IMAGE:
            ret = doc_layout_image_block(layout, doc, i, 0, &y, width);
            y += base.paragraph_gap;
            break;
        case SOLAR_OS_DOC_BLOCK_BLANK:
            ret = doc_layout_blank_block(layout, doc, i, 0, &y);
            break;
        case SOLAR_OS_DOC_BLOCK_PARAGRAPH:
        default:
            ret = doc_layout_wrapped_block(layout, doc, i, 0, &y, width, NULL, layout->zoom, SOLAR_OS_DOC_RUN_PLAIN);
            y += base.paragraph_gap;
            break;
        }

        if (ret != ESP_OK) {
            return ret;
        }
    }

    layout->height = y;
    return ESP_OK;
}

static void doc_render_text_slice_clipped(solar_os_gfx_t *gfx,
                                          const char *text,
                                          size_t len,
                                          int x,
                                          int baseline,
                                          int char_w,
                                          int clip_x,
                                          int clip_w)
{
    if (gfx == NULL || text == NULL || len == 0 || char_w <= 0 || clip_w <= 0) {
        return;
    }

    const int clip_right = clip_x + clip_w;
    if (x >= clip_right) {
        return;
    }

    if (x < clip_x) {
        const int hidden_px = clip_x - x;
        const size_t skip = (size_t)((hidden_px + char_w - 1) / char_w);
        if (skip >= len) {
            return;
        }
        text += skip;
        len -= skip;
        x += (int)skip * char_w;
    }
    if (x >= clip_right) {
        return;
    }

    size_t visible_chars = (size_t)((clip_right - x) / char_w);
    if (visible_chars == 0) {
        return;
    }
    if (visible_chars > len) {
        visible_chars = len;
    }

    char temp[160];
    while (visible_chars > 0) {
        size_t n = visible_chars < sizeof(temp) - 1U ? visible_chars : sizeof(temp) - 1U;
        memcpy(temp, text, n);
        temp[n] = '\0';
        solar_os_gfx_text(gfx, x, baseline, temp);
        text += n;
        visible_chars -= n;
        x += (int)n * char_w;
    }
}

static solar_os_gfx_color_t doc_gray_to_color(uint8_t gray)
{
    const uint8_t level = (uint8_t)(((uint16_t)gray * SOLAR_OS_GFX_GRAY_MAX + 127U) / 255U);
    return solar_os_gfx_gray(level);
}

static bool doc_layout_is_first_line_of_block(const solar_os_doc_layout_t *layout, size_t line_index)
{
    if (layout == NULL || line_index >= layout->line_count) {
        return false;
    }
    if (line_index == 0) {
        return true;
    }
    return layout->lines[line_index - 1U].block_index != layout->lines[line_index].block_index;
}

static bool doc_layout_block_bounds(const solar_os_doc_layout_t *layout,
                                    size_t block_index,
                                    int *x,
                                    int *y,
                                    int *width,
                                    int *height)
{
    if (layout == NULL || layout->line_count == 0) {
        return false;
    }

    bool found = false;
    int min_x = 0;
    int min_y = 0;
    int max_x = 0;
    int max_y = 0;
    for (size_t i = 0; i < layout->line_count; i++) {
        const solar_os_doc_layout_line_t *line = &layout->lines[i];
        if (line->block_index != block_index) {
            continue;
        }
        const int line_right = line->x + line->width;
        const int line_bottom = line->y + line->height;
        if (!found) {
            min_x = line->x;
            min_y = line->y;
            max_x = line_right;
            max_y = line_bottom;
            found = true;
        } else {
            if (line->x < min_x) {
                min_x = line->x;
            }
            if (line->y < min_y) {
                min_y = line->y;
            }
            if (line_right > max_x) {
                max_x = line_right;
            }
            if (line_bottom > max_y) {
                max_y = line_bottom;
            }
        }
    }

    if (!found) {
        return false;
    }
    if (x != NULL) {
        *x = min_x;
    }
    if (y != NULL) {
        *y = min_y;
    }
    if (width != NULL) {
        *width = max_x - min_x;
    }
    if (height != NULL) {
        *height = max_y - min_y;
    }
    return true;
}

static void doc_draw_gray_scaled(solar_os_gfx_t *gfx,
                                 const uint8_t *gray,
                                 int image_w,
                                 int image_h,
                                 int origin_x,
                                 int origin_y,
                                 int draw_w,
                                 int draw_h,
                                 int clip_x,
                                 int clip_y,
                                 int clip_w,
                                 int clip_h)
{
    if (gfx == NULL || gray == NULL || image_w <= 0 || image_h <= 0 || draw_w <= 0 || draw_h <= 0) {
        return;
    }

    int y0 = origin_y < clip_y ? clip_y : origin_y;
    int y1 = origin_y + draw_h;
    const int clip_bottom = clip_y + clip_h;
    const int clip_right = clip_x + clip_w;
    if (y1 > clip_bottom) {
        y1 = clip_bottom;
    }
    int x0 = origin_x < clip_x ? clip_x : origin_x;
    int x1 = origin_x + draw_w;
    if (x1 > clip_right) {
        x1 = clip_right;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int dy = y0; dy < y1; dy++) {
        const int sy = (int)(((int64_t)(dy - origin_y) * image_h) / draw_h);
        solar_os_gfx_color_t run_color = SOLAR_OS_GFX_COLOR_WHITE;
        int run_start = x0;
        bool run_active = false;

        for (int dx = x0; dx < x1; dx++) {
            const int sx = (int)(((int64_t)(dx - origin_x) * image_w) / draw_w);
            const uint8_t value = gray[(size_t)sy * (size_t)image_w + (size_t)sx];
            const solar_os_gfx_color_t color = doc_gray_to_color(value);
            if (!run_active) {
                run_active = true;
                run_color = color;
                run_start = dx;
            } else if (color != run_color) {
                solar_os_gfx_set_color(gfx, run_color);
                solar_os_gfx_fill_rect(gfx, run_start, dy, dx - run_start, 1);
                run_color = color;
                run_start = dx;
            }
        }
        if (run_active) {
            solar_os_gfx_set_color(gfx, run_color);
            solar_os_gfx_fill_rect(gfx, run_start, dy, x1 - run_start, 1);
        }
    }
}

static void doc_render_table_grid(solar_os_gfx_t *gfx,
                                  const solar_os_doc_t *doc,
                                  const solar_os_doc_layout_line_t *line,
                                  const solar_os_doc_view_t *view)
{
    const solar_os_doc_block_t *block = &doc->blocks[line->block_index];
    const size_t cells = doc_table_cell_count(block->text);
    const int x = view->x + line->x;
    const int y = view->y + line->y - view->scroll_y;
    const int w = view->width;
    const int h = line->height;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, w, h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, w, h);
    for (size_t cell = 1; cell < cells; cell++) {
        const int col_x = x + (int)(((int64_t)w * (int64_t)cell) / (int64_t)cells);
        solar_os_gfx_line(gfx, col_x, y, col_x, y + h - 1);
    }
}

static void doc_render_image_block(solar_os_gfx_t *gfx,
                                   const solar_os_doc_t *doc,
                                   const solar_os_doc_layout_line_t *line,
                                   const solar_os_doc_view_t *view)
{
    const solar_os_doc_block_t *block = &doc->blocks[line->block_index];
    const int box_w = line->width > 0 ? line->width : view->width;
    const int box_h = line->height > 8 ? line->height - 8 : line->height;
    const int x = view->x + (view->width - box_w) / 2;
    const int y = view->y + line->y - view->scroll_y + 4;

    if (block->image.gray != NULL && block->image.width > 0 && block->image.height > 0) {
        doc_draw_gray_scaled(gfx,
                             block->image.gray,
                             (int)block->image.width,
                             (int)block->image.height,
                             x,
                             y,
                             box_w,
                             box_h,
                             view->x,
                             view->y,
                             view->width,
                             view->height);
        return;
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, box_w, box_h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, box_w, box_h);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    doc_render_text_slice_clipped(gfx,
                                  block->text != NULL ? block->text : "image",
                                  block->text != NULL ? strlen(block->text) : 5U,
                                  x + 4,
                                  y + 12,
                                  6,
                                  view->x,
                                  view->width);
}

static void doc_render_line_decor(solar_os_gfx_t *gfx,
                                  const solar_os_doc_t *doc,
                                  const solar_os_doc_layout_t *layout,
                                  const solar_os_doc_view_t *view,
                                  size_t line_index)
{
    const solar_os_doc_layout_line_t *line = &layout->lines[line_index];
    const solar_os_doc_block_t *block = &doc->blocks[line->block_index];
    const int screen_y = view->y + line->y - view->scroll_y;

    switch (block->type) {
    case SOLAR_OS_DOC_BLOCK_LIST_ITEM:
        if (doc_layout_is_first_line_of_block(layout, line_index)) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_circle(gfx, view->x + 5, screen_y + (line->height / 2), 2);
        }
        break;
    case SOLAR_OS_DOC_BLOCK_QUOTE:
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
        solar_os_gfx_fill_rect(gfx, view->x, screen_y, 2, line->height);
        break;
    case SOLAR_OS_DOC_BLOCK_RULE:
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
        solar_os_gfx_line(gfx,
                          view->x,
                          screen_y + (line->height / 2),
                          view->x + view->width - 1,
                          screen_y + (line->height / 2));
        break;
    case SOLAR_OS_DOC_BLOCK_TABLE_ROW:
        doc_render_table_grid(gfx, doc, line, view);
        break;
    case SOLAR_OS_DOC_BLOCK_PRE:
        if (doc_layout_is_first_line_of_block(layout, line_index)) {
            int bx = 0;
            int by = 0;
            int bw = 0;
            int bh = 0;
            if (doc_layout_block_bounds(layout, line->block_index, &bx, &by, &bw, &bh)) {
                const int x = view->x + bx - 3;
                const int y = view->y + by - view->scroll_y - 2;
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
                solar_os_gfx_fill_rect(gfx, x, y, view->width - bx, bh + 4);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
                solar_os_gfx_rect(gfx, x, y, view->width - bx, bh + 4);
            }
        }
        break;
    case SOLAR_OS_DOC_BLOCK_IMAGE:
        doc_render_image_block(gfx, doc, line, view);
        break;
    default:
        break;
    }
}

static void doc_render_text_run(solar_os_gfx_t *gfx,
                                const solar_os_doc_layout_run_t *run,
                                const solar_os_doc_view_t *view,
                                const char *text,
                                size_t len,
                                int x,
                                int baseline,
                                int char_w,
                                solar_os_gfx_color_t color)
{
    if (gfx == NULL || run == NULL || view == NULL || text == NULL || len == 0) {
        return;
    }

    solar_os_gfx_set_color(gfx, color);
    doc_render_text_slice_clipped(gfx,
                                  text,
                                  len,
                                  x,
                                  baseline,
                                  char_w,
                                  view->x,
                                  view->width);
}

void solar_os_doc_layout_render(solar_os_gfx_t *gfx,
                                const solar_os_doc_t *doc,
                                const solar_os_doc_layout_t *layout,
                                const solar_os_doc_view_t *view)
{
    if (gfx == NULL || doc == NULL || layout == NULL || view == NULL) {
        return;
    }

    for (size_t i = 0; i < layout->line_count; i++) {
        const solar_os_doc_layout_line_t *line = &layout->lines[i];
        const int screen_y = view->y + line->y - view->scroll_y;
        if (screen_y + line->height < view->y) {
            continue;
        }
        if (screen_y > view->y + view->height) {
            break;
        }

        doc_render_line_decor(gfx, doc, layout, view, i);

        for (size_t r = 0; r < line->run_count; r++) {
            const solar_os_doc_layout_run_t *run = &layout->runs[line->run_start + r];
            const int draw_x = view->x + run->x;
            const int draw_y = view->y + run->y - view->scroll_y;
            if (draw_x >= view->x + view->width || draw_x + run->width < view->x) {
                continue;
            }

            int char_w = run->char_w > 0 ? run->char_w : doc_base_style_for(layout->zoom).char_w;

            solar_os_gfx_set_font(gfx, run->font);
            const solar_os_gfx_color_t text_color =
                (run->style & SOLAR_OS_DOC_RUN_ITALIC) != 0 &&
                        (run->style & SOLAR_OS_DOC_RUN_BOLD) == 0 ?
                    SOLAR_OS_GFX_COLOR_DARK :
                    SOLAR_OS_GFX_COLOR_BLACK;
            solar_os_gfx_set_color(gfx, text_color);

            if ((run->style & SOLAR_OS_DOC_RUN_CODE) != 0 &&
                doc->blocks[run->block_index].type != SOLAR_OS_DOC_BLOCK_PRE &&
                run->width > 0) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
                solar_os_gfx_fill_rect(gfx,
                                       draw_x - 1,
                                       draw_y + 1,
                                       run->width + 2,
                                       run->height - 2);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
                solar_os_gfx_rect(gfx,
                                  draw_x - 1,
                                  draw_y + 1,
                                  run->width + 2,
                                  run->height - 2);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            }
            if (run->text_offset == SIZE_MAX) {
                doc_render_text_run(gfx,
                                    run,
                                    view,
                                    run->literal,
                                    strlen(run->literal),
                                    draw_x,
                                    draw_y + run->baseline,
                                    char_w,
                                    text_color);
                continue;
            }

            const solar_os_doc_block_t *block = &doc->blocks[run->block_index];
            const char *text = block->text != NULL ? block->text : "";
            doc_render_text_run(gfx,
                                run,
                                view,
                                &text[run->text_offset],
                                run->text_len,
                                draw_x,
                                draw_y + run->baseline,
                                char_w,
                                text_color);
            if ((run->style & SOLAR_OS_DOC_RUN_UNDERLINE) != 0 && run->width > 0) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_line(gfx,
                                  draw_x,
                                  draw_y + run->baseline + 2,
                                  draw_x + run->width - 1,
                                  draw_y + run->baseline + 2);
            }
        }
    }
}

bool solar_os_doc_layout_source_to_xy(const solar_os_doc_layout_t *layout,
                                      size_t source_offset,
                                      int *x,
                                      int *y,
                                      int *height)
{
    if (layout == NULL || layout->line_count == 0) {
        return false;
    }

    const solar_os_doc_layout_run_t *best = NULL;
    for (size_t i = 0; i < layout->run_count; i++) {
        const solar_os_doc_layout_run_t *run = &layout->runs[i];
        if (run->text_offset == SIZE_MAX || run->source_start == SIZE_MAX) {
            continue;
        }
        if (source_offset >= run->source_start && source_offset <= run->source_end) {
            const size_t span = run->source_end > run->source_start ?
                run->source_end - run->source_start :
                1U;
            size_t pos = source_offset - run->source_start;
            if (pos > span) {
                pos = span;
            }
            if (pos > run->text_len) {
                pos = run->text_len;
            }
            const int char_w = run->char_w > 0 ? run->char_w : doc_base_style_for(layout->zoom).char_w;
            if (x != NULL) {
                *x = run->x + ((int)pos * char_w);
            }
            if (y != NULL) {
                *y = run->y;
            }
            if (height != NULL) {
                *height = run->height;
            }
            return true;
        }
        if (run->source_end <= source_offset) {
            best = run;
        }
    }

    if (best != NULL) {
        if (x != NULL) {
            *x = best->x + best->width;
        }
        if (y != NULL) {
            *y = best->y;
        }
        if (height != NULL) {
            *height = best->height;
        }
        return true;
    }

    if (x != NULL) {
        *x = layout->lines[0].x;
    }
    if (y != NULL) {
        *y = layout->lines[0].y;
    }
    if (height != NULL) {
        *height = layout->lines[0].height;
    }
    return true;
}

bool solar_os_doc_layout_hit_test(const solar_os_doc_layout_t *layout,
                                  int x,
                                  int y,
                                  size_t *source_offset)
{
    if (layout == NULL || source_offset == NULL || layout->line_count == 0) {
        return false;
    }

    const solar_os_doc_layout_line_t *line = &layout->lines[0];
    for (size_t i = 0; i < layout->line_count; i++) {
        const solar_os_doc_layout_line_t *candidate = &layout->lines[i];
        if (y >= candidate->y && y < candidate->y + candidate->height) {
            line = candidate;
            break;
        }
        if (y >= candidate->y) {
            line = candidate;
        }
    }

    const solar_os_doc_layout_run_t *last = NULL;
    for (size_t r = 0; r < line->run_count; r++) {
        const solar_os_doc_layout_run_t *run = &layout->runs[line->run_start + r];
        if (run->text_offset == SIZE_MAX) {
            continue;
        }
        last = run;
        const size_t source_span = run->source_end > run->source_start ?
            run->source_end - run->source_start :
            run->text_len;
        const size_t visual_span = run->text_len > 0 ? run->text_len : source_span;
        const int char_w = run->char_w > 0 ? run->char_w : doc_base_style_for(layout->zoom).char_w;
        const int hit_width = run->width > 0 ? run->width : (int)visual_span * char_w;
        if (x < run->x + hit_width) {
            int rel = x - run->x;
            if (rel < 0) {
                rel = 0;
            }
            size_t pos = char_w > 0 ? (size_t)((rel + (char_w / 2)) / char_w) : 0;
            if (pos > visual_span) {
                pos = visual_span;
            }
            if (pos > source_span) {
                pos = source_span;
            }
            *source_offset = run->source_start + pos;
            return true;
        }
    }

    if (last != NULL) {
        const size_t source_span = last->source_end > last->source_start ?
            last->source_end - last->source_start :
            0U;
        size_t visual_span = last->text_len;
        if (visual_span > source_span) {
            visual_span = source_span;
        }
        *source_offset = last->source_start + visual_span;
        return true;
    }
    if (line->source_start != SIZE_MAX) {
        *source_offset = line->source_start;
        return true;
    }
    *source_offset = 0;
    return true;
}

int solar_os_doc_measure_height(const solar_os_doc_t *doc, int width, int zoom)
{
    solar_os_doc_layout_t layout;
    solar_os_doc_layout_init(&layout);
    const esp_err_t ret = solar_os_doc_layout_build(&layout, doc, width, zoom);
    const int height = ret == ESP_OK ? layout.height : 0;
    solar_os_doc_layout_free(&layout);
    return height;
}

void solar_os_doc_render(solar_os_gfx_t *gfx,
                         const solar_os_doc_t *doc,
                         const solar_os_doc_view_t *view)
{
    if (gfx == NULL || doc == NULL || view == NULL || view->width <= 0 || view->height <= 0) {
        return;
    }

    solar_os_doc_layout_t layout;
    solar_os_doc_layout_init(&layout);
    if (solar_os_doc_layout_build(&layout, doc, view->width, view->zoom) == ESP_OK) {
        solar_os_doc_layout_render(gfx, doc, &layout, view);
    }
    solar_os_doc_layout_free(&layout);
}
