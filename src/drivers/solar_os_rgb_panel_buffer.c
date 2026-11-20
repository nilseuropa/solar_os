#include "solar_os_rgb_panel_buffer.h"
#include "solar_os_indexed_raster.h"
#include <string.h>

static bool valid(const solar_os_rgb_panel_buffer_t *buffer)
{
    return buffer && buffer->pixels && buffer->width && buffer->height;
}

static void rotate_put(solar_os_rgb_panel_buffer_t *b, uint16_t x, uint16_t y,
    uint16_t color, solar_os_display_rotation_t rotation)
{
    switch (rotation) {
    case SOLAR_OS_DISPLAY_ROTATION_90: solar_os_rgb_panel_put(b, b->width - 1U - y, x, color); break;
    case SOLAR_OS_DISPLAY_ROTATION_180: solar_os_rgb_panel_put(b, b->width - 1U - x, b->height - 1U - y, color); break;
    case SOLAR_OS_DISPLAY_ROTATION_270: solar_os_rgb_panel_put(b, y, b->height - 1U - x, color); break;
    default: solar_os_rgb_panel_put(b, x, y, color); break;
    }
}

void solar_os_rgb_panel_clear(solar_os_rgb_panel_buffer_t *buffer, uint16_t color)
{
    if (!valid(buffer)) return;
    for (uint16_t y = 0; y < buffer->height; ++y)
        for (uint16_t x = 0; x < buffer->width; ++x) solar_os_rgb_panel_put(buffer, x, y, color);
}

esp_err_t solar_os_rgb_panel_frame(solar_os_rgb_panel_buffer_t *b,
    const solar_os_display_raster_t *f, solar_os_display_rotation_t rotation)
{
    if (!valid(b) || !f || !f->data || !f->source_width || !f->source_height ||
        !f->width || !f->height || (unsigned)rotation > SOLAR_OS_DISPLAY_ROTATION_270)
        return ESP_ERR_INVALID_ARG;
    bool rgb = f->format == SOLAR_OS_DISPLAY_FORMAT_RGB565;
    bool indexed8 = f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8;
    if (!rgb && !indexed8 && f->format != SOLAR_OS_DISPLAY_FORMAT_INDEX2)
        return ESP_ERR_NOT_SUPPORTED;
    size_t stride = rgb ? (size_t)f->source_width * 2U :
        (indexed8 ? f->source_width : (f->source_width + 3U) / 4U);
    if (f->source_stride < stride || f->data_size < (size_t)f->source_stride * f->source_height ||
        (rgb && f->palette_inverted) || (!rgb && (!f->palette_rgb565 ||
        f->palette_size < (indexed8 ? 256U : 4U))) ||
        (f->clear_background && !rgb && !indexed8 && f->background_index >= 4U))
        return ESP_ERR_INVALID_ARG;
    bool quarter = rotation == SOLAR_OS_DISPLAY_ROTATION_90 || rotation == SOLAR_OS_DISPLAY_ROTATION_270;
    uint16_t width = quarter ? b->height : b->width, height = quarter ? b->width : b->height;
    if ((uint32_t)f->x + f->width > width || (uint32_t)f->y + f->height > height)
        return ESP_ERR_INVALID_SIZE;
    uint16_t colors[256];
    if (!rgb) solar_os_indexed_palette(f->palette_rgb565, f->format, f->palette_inverted, colors);
    if (f->clear_background) solar_os_rgb_panel_clear(b,
        rgb ? f->background_rgb565 : colors[f->background_index]);
    /* Video decoders already provide a sized RGB565 raster in wire order. */
    if (rgb && rotation == SOLAR_OS_DISPLAY_ROTATION_0 &&
        f->source_width == f->width && f->source_height == f->height) {
        const size_t bytes = (size_t)f->width * 2U;
        for (uint16_t y = 0; y < f->height; y++) {
            const uint8_t *source = f->data + (size_t)y * f->source_stride;
            const unsigned row = f->y + y;
            uint8_t *destination = b->pixels + ((size_t)row * b->width + f->x) * 2U;
            if (memcmp(destination, source, bytes) == 0) continue;
            memcpy(destination, source, bytes);
            if (b->dirty_rows) b->dirty_rows[row / 8U] |= 1U << (row & 7U);
        }
        return ESP_OK;
    }
    for (uint16_t y = 0; y < f->height; ++y) {
        uint16_t sy = (uint32_t)y * f->source_height / f->height;
        const uint8_t *row = f->data + (size_t)sy * f->source_stride;
        for (uint16_t x = 0; x < f->width; ++x) {
            uint16_t sx = (uint32_t)x * f->source_width / f->width;
            uint16_t color = rgb ? ((uint16_t)row[sx * 2U] << 8) | row[sx * 2U + 1U] :
                colors[solar_os_indexed_pixel(row, sx, f->format)];
            rotate_put(b, f->x + x, f->y + y, color, rotation);
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_rgb_panel_surface_update(solar_os_rgb_panel_buffer_t *b,
    const solar_os_display_surface_t *s, bool changed_only)
{
    if (!valid(b) || !s || !s->data || !s->width || !s->height ||
        s->format != SOLAR_OS_DISPLAY_FORMAT_INDEX8 || !s->palette_rgb565 ||
        s->palette_size < 256U || s->stride < s->width ||
        s->data_size < (size_t)s->stride * s->height ||
        (unsigned)s->rotation > SOLAR_OS_DISPLAY_ROTATION_270)
        return ESP_ERR_INVALID_ARG;
    bool quarter = s->rotation == SOLAR_OS_DISPLAY_ROTATION_90 || s->rotation == SOLAR_OS_DISPLAY_ROTATION_270;
    if (s->native_width != b->width || s->native_height != b->height ||
        s->width != (quarter ? b->height : b->width) ||
        s->height != (quarter ? b->width : b->height)) return ESP_ERR_INVALID_SIZE;
    const unsigned columns = (b->width + 7U) / 8U, rows = (b->height + 7U) / 8U;
    const bool hashes = s->presented_hashes && s->hash_stride >= columns &&
        s->presented_hash_count >= (size_t)s->hash_stride * rows;
    if ((changed_only || hashes) && s->dirty_tiles && s->tile_size == 8U &&
        s->dirty_stride >= (columns + 7U) / 8U &&
        s->dirty_size >= (size_t)s->dirty_stride * rows) {
        for (unsigned ty = 0; ty < rows; ty++) for (unsigned tx = 0; tx < columns; tx++) {
            if (changed_only && !(s->dirty_tiles[(size_t)ty * s->dirty_stride + tx / 8U] & (1U << (tx & 7U)))) continue;
            const unsigned end_x = (tx + 1U) * 8U < b->width ? (tx + 1U) * 8U : b->width;
            const unsigned end_y = (ty + 1U) * 8U < b->height ? (ty + 1U) * 8U : b->height;
            uint8_t indices[64];
            unsigned count = 0;
            uint32_t hash = UINT32_C(2166136261);
            for (unsigned ny = ty * 8U; ny < end_y; ny++) for (unsigned nx = tx * 8U; nx < end_x; nx++) {
                unsigned x = nx, y = ny;
                switch (s->rotation) {
                case SOLAR_OS_DISPLAY_ROTATION_90: x = ny; y = b->width - 1U - nx; break;
                case SOLAR_OS_DISPLAY_ROTATION_180: x = b->width - 1U - nx; y = b->height - 1U - ny; break;
                case SOLAR_OS_DISPLAY_ROTATION_270: x = b->height - 1U - ny; y = nx; break;
                default: break;
                }
                const uint8_t index = s->data[(size_t)y * s->stride + x];
                indices[count++] = index;
                hash = (hash ^ index) * UINT32_C(16777619);
            }
            const size_t hash_index = (size_t)ty * s->hash_stride + tx;
            if (hashes && changed_only && s->presented_hashes[hash_index] == hash) continue;
            count = 0;
            for (unsigned ny = ty * 8U; ny < end_y; ny++) for (unsigned nx = tx * 8U; nx < end_x; nx++)
                solar_os_rgb_panel_put(b, nx, ny, s->palette_rgb565[indices[count++]]);
            if (hashes) s->presented_hashes[hash_index] = hash;
        }
        return ESP_OK;
    }
    for (uint16_t y = 0; y < s->height; ++y)
        for (uint16_t x = 0; x < s->width; ++x)
            rotate_put(b, x, y, s->palette_rgb565[s->data[(size_t)y * s->stride + x]], s->rotation);
    return ESP_OK;
}

esp_err_t solar_os_rgb_panel_surface(solar_os_rgb_panel_buffer_t *b,
    const solar_os_display_surface_t *s)
{
    return solar_os_rgb_panel_surface_update(b, s, false);
}
