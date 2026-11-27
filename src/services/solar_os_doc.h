#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_gfx.h"
#include "solar_os_storage.h"
#include "solar_os_paged_array.h"

#include "solar_os_native_document_abi.h"
#include "solar_os_native_document_layout_abi.h"

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
    solar_os_doc_block_type_t type;
    uint8_t level;
    char list_delimiter; /* '.' or ')' for numbered lists; zero for bullets. */
    uint32_t list_number;
    size_t raw_source_start;
    size_t raw_source_end;
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
    /* Borrowed document; keep its text unchanged until the layout is rebuilt. */
    const solar_os_doc_t *doc;
    solar_os_paged_array_t lines;
    size_t line_count;
    size_t line_capacity;
    solar_os_paged_array_t runs;
    size_t run_count;
    size_t run_capacity;
    int width;
    int height;
    int zoom;
} solar_os_doc_layout_t;

static inline solar_os_doc_layout_line_t *solar_os_doc_layout_line(
    const solar_os_doc_layout_t *layout, size_t index)
{
    return solar_os_paged_array_at(&layout->lines, index);
}

static inline solar_os_doc_layout_run_t *solar_os_doc_layout_run(
    const solar_os_doc_layout_t *layout, size_t index)
{
    return solar_os_paged_array_at(&layout->runs, index);
}

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
esp_err_t solar_os_doc_layout_build_ex(solar_os_doc_layout_t *layout,
                                       const solar_os_doc_t *doc,
                                       int width,
                                       int zoom,
                                       const solar_os_doc_reveal_range_t *reveal_ranges,
                                       size_t reveal_range_count);
int solar_os_doc_layout_run_source_x(const solar_os_doc_layout_t *layout,
                                      const solar_os_doc_layout_run_t *run,
                                      size_t source_offset);
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
bool solar_os_doc_layout_adjacent_source(const solar_os_doc_layout_t *layout,
                                         size_t source_offset,
                                         bool down,
                                         size_t *target_offset);
int solar_os_doc_measure_height(const solar_os_doc_t *doc, int width, int zoom);
void solar_os_doc_render(solar_os_gfx_t *gfx,
                         const solar_os_doc_t *doc,
                         const solar_os_doc_view_t *view);
