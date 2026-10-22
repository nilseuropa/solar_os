#include "rgb_lcd_raster.h"

static esp_err_t validate(const solar_os_display_raster_t *f)
{
    if (!f || !f->data || !f->source_width || !f->source_height || !f->width || !f->height)
        return ESP_ERR_INVALID_ARG;
    size_t row;
    switch (f->format) {
    case SOLAR_OS_DISPLAY_FORMAT_MONO1: row = (f->source_width + 7U) / 8U; break;
    case SOLAR_OS_DISPLAY_FORMAT_INDEX2: row = (f->source_width + 3U) / 4U; break;
    case SOLAR_OS_DISPLAY_FORMAT_INDEX8: row = f->source_width; break;
    case SOLAR_OS_DISPLAY_FORMAT_RGB565: row = (size_t)f->source_width * 2U; break;
    default: return ESP_ERR_NOT_SUPPORTED;
    }
    if (f->source_stride < row ||
        (size_t)(f->source_height - 1U) * f->source_stride + row > f->data_size)
        return ESP_ERR_INVALID_SIZE;
    if (f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 || f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8) {
        if (!f->palette_rgb565 || !f->palette_size ||
            (f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 && f->palette_size < 4U))
            return ESP_ERR_INVALID_ARG;
        if (f->clear_background && (f->background_index >= f->palette_size ||
            (f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 && f->background_index >= 4U)))
            return ESP_ERR_INVALID_ARG;
        for (unsigned y = 0; y < f->source_height; y++) {
            const uint8_t *p = f->data + y * (size_t)f->source_stride;
            for (unsigned x = 0; x < f->source_width; x++) {
                unsigned index = f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? p[x] :
                    (p[x / 4U] >> ((x % 4U) * 2U)) & 3U;
                if (index >= f->palette_size) return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

static uint16_t indexed_color(const solar_os_display_raster_t *f, unsigned index)
{
    if (f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX2)
        return f->palette_rgb565[f->palette_inverted ? 3U - index : index];
    const uint16_t color = f->palette_rgb565[index];
    return f->palette_inverted ? (uint16_t)~color : color;
}

static uint16_t pixel(const solar_os_display_raster_t *f, unsigned x, unsigned y,
                       uint16_t foreground, uint16_t background)
{
    const uint8_t *row = f->data + (size_t)y * f->source_stride;
    switch (f->format) {
    case SOLAR_OS_DISPLAY_FORMAT_RGB565:
        return ((uint16_t)row[x * 2U] << 8U) | row[x * 2U + 1U];
    case SOLAR_OS_DISPLAY_FORMAT_MONO1:
        return ((((row[x / 8U] >> (x % 8U)) & 1U) != 0) ^ f->palette_inverted) ? foreground : background;
    case SOLAR_OS_DISPLAY_FORMAT_INDEX2:
        return indexed_color(f, (row[x / 4U] >> ((x % 4U) * 2U)) & 3U);
    default: return indexed_color(f, row[x]);
    }
}

esp_err_t solar_os_rgb_lcd_render(uint16_t *out, uint16_t width, uint16_t height,
    const solar_os_display_raster_t *f, uint16_t foreground, uint16_t background)
{
    if (!out || !f || (unsigned)f->x + f->width > width ||
        (unsigned)f->y + f->height > height) return ESP_ERR_INVALID_ARG;
    const esp_err_t err = validate(f);
    if (err != ESP_OK) return err;
    if (f->clear_background) {
        uint16_t clear = background;
        if (f->format == SOLAR_OS_DISPLAY_FORMAT_RGB565) clear = f->background_rgb565;
        else if (f->format == SOLAR_OS_DISPLAY_FORMAT_MONO1)
            clear = ((f->background_index != 0) ^ f->palette_inverted) ? foreground : background;
        else clear = indexed_color(f, f->background_index);
        for (size_t i = 0; i < (size_t)width * height; i++) out[i] = clear;
    }
    for (unsigned y = 0; y < f->height; y++) {
        const unsigned sy = y * (uint32_t)f->source_height / f->height;
        for (unsigned x = 0; x < f->width; x++) {
            const unsigned sx = x * (uint32_t)f->source_width / f->width;
            out[(size_t)(f->y + y) * width + f->x + x] = pixel(f, sx, sy, foreground, background);
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_rgb_lcd_render_surface(uint16_t *out, uint16_t width, uint16_t height,
    const solar_os_display_surface_t *s, uint16_t foreground, uint16_t background)
{
    if (!out || !s || s->rotation > SOLAR_OS_DISPLAY_ROTATION_270 ||
        s->native_width != width || s->native_height != height) return ESP_ERR_INVALID_ARG;
    const bool swap = s->rotation == SOLAR_OS_DISPLAY_ROTATION_90 || s->rotation == SOLAR_OS_DISPLAY_ROTATION_270;
    if (s->width != (swap ? height : width) || s->height != (swap ? width : height))
        return ESP_ERR_INVALID_ARG;
    const solar_os_display_raster_t f = {
        .data=s->data, .data_size=s->data_size, .source_width=s->width,
        .source_height=s->height, .source_stride=s->stride, .width=s->width,
        .height=s->height, .format=s->format, .palette_rgb565=s->palette_rgb565,
        .palette_size=s->palette_size,
    };
    const esp_err_t err = validate(&f);
    if (err != ESP_OK) return err;
    for (unsigned y = 0; y < s->height; y++) {
        for (unsigned x = 0; x < s->width; x++) {
            unsigned dx=x, dy=y;
            if (s->rotation == SOLAR_OS_DISPLAY_ROTATION_90) { dx=width-1U-y; dy=x; }
            else if (s->rotation == SOLAR_OS_DISPLAY_ROTATION_180) { dx=width-1U-x; dy=height-1U-y; }
            else if (s->rotation == SOLAR_OS_DISPLAY_ROTATION_270) { dx=y; dy=height-1U-x; }
            out[(size_t)dy * width + dx] = pixel(&f, x, y, foreground, background);
        }
    }
    return ESP_OK;
}
