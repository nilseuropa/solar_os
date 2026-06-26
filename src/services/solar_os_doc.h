#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_gfx.h"
#include "solar_os_storage.h"

typedef enum {
    SOLAR_OS_DOC_BLOCK_PARAGRAPH,
    SOLAR_OS_DOC_BLOCK_HEADING,
    SOLAR_OS_DOC_BLOCK_LIST_ITEM,
    SOLAR_OS_DOC_BLOCK_QUOTE,
    SOLAR_OS_DOC_BLOCK_PRE,
    SOLAR_OS_DOC_BLOCK_RULE,
    SOLAR_OS_DOC_BLOCK_TABLE_ROW,
    SOLAR_OS_DOC_BLOCK_IMAGE,
    SOLAR_OS_DOC_BLOCK_BLANK,
} solar_os_doc_block_type_t;

typedef enum {
    SOLAR_OS_DOC_RUN_PLAIN = 0,
    SOLAR_OS_DOC_RUN_BOLD = 1U << 0,
    SOLAR_OS_DOC_RUN_ITALIC = 1U << 1,
    SOLAR_OS_DOC_RUN_CODE = 1U << 2,
    SOLAR_OS_DOC_RUN_LINK = 1U << 3,
    SOLAR_OS_DOC_RUN_UNDERLINE = 1U << 4,
} solar_os_doc_run_style_t;

typedef esp_err_t (*solar_os_doc_asset_read_cb_t)(void *user,
                                                  const char *document_path,
                                                  const char *target,
                                                  uint8_t **out_data,
                                                  size_t *out_len);
typedef void (*solar_os_doc_asset_release_cb_t)(void *user, uint8_t *data);

typedef struct {
    solar_os_doc_asset_read_cb_t read;
    solar_os_doc_asset_release_cb_t release;
    void *user;
} solar_os_doc_asset_provider_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t *gray;
    esp_err_t status;
    bool attempted;
} solar_os_doc_image_t;

typedef struct {
    size_t text_offset;
    size_t text_len;
    size_t source_start;
    size_t source_end;
    uint8_t style;
} solar_os_doc_run_t;

typedef struct {
    solar_os_doc_block_type_t type;
    uint8_t level;
    size_t source_start;
    size_t source_end;
    size_t run_start;
    size_t run_count;
    char *text;
    solar_os_doc_image_t image;
} solar_os_doc_block_t;

typedef struct {
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char *source;
    size_t source_len;
    solar_os_doc_block_t *blocks;
    size_t block_count;
    size_t block_capacity;
    solar_os_doc_run_t *runs;
    size_t run_count;
    size_t run_capacity;
    solar_os_doc_asset_provider_t assets;
} solar_os_doc_t;

typedef struct {
    int x;
    int y;
    int width;
    int height;
    int scroll_y;
    int zoom;
} solar_os_doc_view_t;

typedef struct {
    int x;
    int y;
    int width;
    int height;
    int baseline;
    size_t block_index;
    size_t run_index;
    size_t text_offset;
    size_t text_len;
    size_t source_start;
    size_t source_end;
    uint8_t style;
    solar_os_gfx_font_t font;
    int char_w;
    uint16_t pixel_size;
    uint8_t face;
    char literal[8];
} solar_os_doc_layout_run_t;

typedef struct {
    int x;
    int y;
    int width;
    int height;
    int baseline;
    size_t block_index;
    size_t run_start;
    size_t run_count;
    size_t source_start;
    size_t source_end;
} solar_os_doc_layout_line_t;

typedef struct {
    solar_os_doc_layout_line_t *lines;
    size_t line_count;
    size_t line_capacity;
    solar_os_doc_layout_run_t *runs;
    size_t run_count;
    size_t run_capacity;
    int width;
    int height;
    int zoom;
} solar_os_doc_layout_t;

void solar_os_doc_init(solar_os_doc_t *doc);
void solar_os_doc_free(solar_os_doc_t *doc);
void solar_os_doc_set_asset_provider(solar_os_doc_t *doc,
                                     const solar_os_doc_asset_provider_t *provider);
esp_err_t solar_os_doc_load_path(solar_os_doc_t *doc, const char *path);
esp_err_t solar_os_doc_load_path_as(solar_os_doc_t *doc, const char *path, bool markdown);
esp_err_t solar_os_doc_parse_markdown(solar_os_doc_t *doc,
                                      const char *source,
                                      size_t source_len,
                                      const char *path);
esp_err_t solar_os_doc_parse_text(solar_os_doc_t *doc,
                                  const char *source,
                                  size_t source_len,
                                  const char *path);
esp_err_t solar_os_doc_write_markdown(const solar_os_doc_t *doc, const char *path);
void solar_os_doc_layout_init(solar_os_doc_layout_t *layout);
void solar_os_doc_layout_free(solar_os_doc_layout_t *layout);
esp_err_t solar_os_doc_layout_build(solar_os_doc_layout_t *layout,
                                    const solar_os_doc_t *doc,
                                    int width,
                                    int zoom);
void solar_os_doc_layout_render(solar_os_gfx_t *gfx,
                                const solar_os_doc_t *doc,
                                const solar_os_doc_layout_t *layout,
                                const solar_os_doc_view_t *view);
bool solar_os_doc_layout_source_to_xy(const solar_os_doc_layout_t *layout,
                                      size_t source_offset,
                                      int *x,
                                      int *y,
                                      int *height);
bool solar_os_doc_layout_hit_test(const solar_os_doc_layout_t *layout,
                                  int x,
                                  int y,
                                  size_t *source_offset);
int solar_os_doc_measure_height(const solar_os_doc_t *doc, int width, int zoom);
void solar_os_doc_render(solar_os_gfx_t *gfx,
                         const solar_os_doc_t *doc,
                         const solar_os_doc_view_t *view);
