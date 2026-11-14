#include "solar_os_rgb_panel_buffer.h"
#include "solar_os_indexed_raster.h"

static bool valid(const solar_os_rgb_panel_buffer_t *buffer)
{
    return buffer && buffer->pixels && buffer->width && buffer->height;
}

static void put(solar_os_rgb_panel_buffer_t *b, uint16_t x, uint16_t y, uint16_t color)
{
    uint8_t *p = b->pixels + ((size_t)y * b->width + x) * 2U;
    p[0] = (uint8_t)(color >> 8); p[1] = (uint8_t)color;
}

static void rotate_put(solar_os_rgb_panel_buffer_t *b, uint16_t x, uint16_t y,
    uint16_t color, solar_os_display_rotation_t rotation)
{
    switch (rotation) {
    case SOLAR_OS_DISPLAY_ROTATION_90: put(b, b->width - 1U - y, x, color); break;
    case SOLAR_OS_DISPLAY_ROTATION_180: put(b, b->width - 1U - x, b->height - 1U - y, color); break;
    case SOLAR_OS_DISPLAY_ROTATION_270: put(b, y, b->height - 1U - x, color); break;
    default: put(b, x, y, color); break;
    }
}

void solar_os_rgb_panel_clear(solar_os_rgb_panel_buffer_t *buffer, uint16_t color)
{
    if (!valid(buffer)) return;
    for (uint16_t y = 0; y < buffer->height; ++y)
        for (uint16_t x = 0; x < buffer->width; ++x) put(buffer, x, y, color);
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

esp_err_t solar_os_rgb_panel_surface(solar_os_rgb_panel_buffer_t *b,
    const solar_os_display_surface_t *s)
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
    for (uint16_t y = 0; y < s->height; ++y)
        for (uint16_t x = 0; x < s->width; ++x)
            rotate_put(b, x, y, s->palette_rgb565[s->data[(size_t)y * s->stride + x]], s->rotation);
    return ESP_OK;
}
