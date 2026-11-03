#include "solar_os_display_layer.h"

#include <string.h>
#include "solar_os_memory.h"

struct solar_os_display_layer {
    u8g2_t *canvas;
    uint8_t *original, *base, *composed, *bits, *raster, *rgb;
    size_t mono_size, stride;
    uint16_t width, height, x, y, w, h;
    uint16_t colors[2]; /* Background and foreground roles of the mono canvas. */
    bool black_is_one, color, base_is_canvas;
};

static void native_pixel(const solar_os_display_layer_t *l, unsigned x, unsigned y,
    unsigned *nx, unsigned *ny)
{
    const u8x8_display_info_t *info = l->canvas->u8x8.display_info;
    *nx = x; *ny = y;
    if (l->canvas->cb == U8G2_R1) { *nx = info->pixel_width - 1U - y; *ny = x; }
    else if (l->canvas->cb == U8G2_R2) { *nx = info->pixel_width - 1U - x; *ny = info->pixel_height - 1U - y; }
    else if (l->canvas->cb == U8G2_R3) { *nx = y; *ny = info->pixel_height - 1U - x; }
}

static bool canvas_black(const solar_os_display_layer_t *l, unsigned x, unsigned y)
{
    unsigned nx, ny;
    native_pixel(l, x, y, &nx, &ny);
    return ((l->base[(ny / 8U) * l->stride + nx] >> (ny & 7U)) & 1U) == l->black_is_one;
}

static void canvas_set(const solar_os_display_layer_t *l, uint8_t *buffer,
    unsigned x, unsigned y, bool black)
{
    unsigned nx, ny;
    native_pixel(l, x, y, &nx, &ny);
    uint8_t *byte = buffer + (ny / 8U) * l->stride + nx;
    uint8_t mask = (uint8_t)(1U << (ny & 7U));
    if (black == l->black_is_one) *byte |= mask;
    else *byte &= (uint8_t)~mask;
}

static uint16_t rgb_read(const uint8_t *p) { return (uint16_t)((p[0] << 8U) | p[1]); }
static void rgb_write(uint8_t *p, uint16_t c) { p[0] = c >> 8U; p[1] = c; }

static void canvas_to_rgb(solar_os_display_layer_t *l)
{
    for (unsigned y = 0; y < l->height; y++)
        for (unsigned x = 0; x < l->width; x++)
            rgb_write(l->rgb + ((size_t)y * l->width + x) * 2U,
                l->colors[canvas_black(l, x, y)]);
}

void solar_os_display_layer_set_colors(solar_os_display_layer_t *l,
    uint16_t foreground, uint16_t background)
{
    if (l->colors[0] == background && l->colors[1] == foreground) return;
    l->colors[0] = background; l->colors[1] = foreground;
    if (l->color && l->base_is_canvas) canvas_to_rgb(l);
}

solar_os_display_layer_t *solar_os_display_layer_create(u8g2_t *canvas,
    bool black_is_one, bool color, uint16_t foreground, uint16_t background)
{
    if (!canvas || !u8g2_GetBufferPtr(canvas) || !canvas->u8x8.display_info ||
        canvas->ll_hvline != u8g2_ll_hvline_vertical_top_lsb ||
        u8g2_GetBufferTileHeight(canvas) != canvas->u8x8.display_info->tile_height ||
        !u8g2_GetDisplayWidth(canvas) || !u8g2_GetDisplayHeight(canvas)) return NULL;
    solar_os_display_layer_t *l = solar_os_memory_calloc(1, sizeof(*l),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "display.layer");
    if (!l) return NULL;
    l->canvas = canvas; l->black_is_one = black_is_one; l->color = color;
    l->colors[0] = background; l->colors[1] = foreground; l->base_is_canvas = true;
    l->width = u8g2_GetDisplayWidth(canvas); l->height = u8g2_GetDisplayHeight(canvas);
    l->stride = (size_t)u8g2_GetBufferTileWidth(canvas) * 8U;
    l->mono_size = l->stride * u8g2_GetBufferTileHeight(canvas);
    const size_t pixels = (size_t)l->width * l->height;
    const size_t bitmap_size = ((l->width + 7U) / 8U) * l->height;
    l->base = solar_os_memory_alloc(l->mono_size * 2U + bitmap_size * 2U +
        (color ? pixels * 4U : 0U), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "display.layer.pixels");
    if (!l->base) { solar_os_memory_free(l); return NULL; }
    l->composed = l->base + l->mono_size;
    l->bits = l->composed + l->mono_size;
    l->raster = l->bits + bitmap_size;
    if (color) { l->rgb = l->raster + bitmap_size; l->raster = l->rgb + pixels * 2U; }
    l->original = u8g2_GetBufferPtr(canvas);
    memcpy(l->base, l->original, l->mono_size);
    canvas->tile_buf_ptr = l->base;
    if (color) canvas_to_rgb(l);
    return l;
}

void solar_os_display_layer_destroy(solar_os_display_layer_t *l)
{
    if (!l) return;
    memcpy(l->original, l->base, l->mono_size);
    l->canvas->tile_buf_ptr = l->original;
    solar_os_memory_free(l->base);
    solar_os_memory_free(l);
}

uint8_t *solar_os_display_layer_base(solar_os_display_layer_t *l) { return l->base; }
void solar_os_display_layer_uncover(solar_os_display_layer_t *l) { l->w = l->h = 0; }

void solar_os_display_layer_update(solar_os_display_layer_t *l, const uint8_t *bits,
    uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    l->x = x; l->y = y; l->w = w; l->h = h;
    memcpy(l->bits, bits, (size_t)((w + 7U) / 8U) * h);
}

static bool layer_black(const solar_os_display_layer_t *l, unsigned x, unsigned y)
{
    return (l->bits[(size_t)(y - l->y) * ((l->w + 7U) / 8U) + (x - l->x) / 8U]
        & (1U << ((x - l->x) & 7U))) != 0;
}

uint8_t *solar_os_display_layer_canvas(solar_os_display_layer_t *l, bool base_changed)
{
    memcpy(l->composed, l->base, l->mono_size);
    for (unsigned y = l->y; y < l->y + l->h; y++)
        for (unsigned x = l->x; x < l->x + l->w; x++)
            canvas_set(l, l->composed, x, y, layer_black(l, x, y));
    if (base_changed) {
        l->base_is_canvas = true;
        if (l->color) canvas_to_rgb(l);
    }
    return l->composed;
}

static uint16_t frame_color(const solar_os_display_raster_t *f, unsigned x, unsigned y)
{
    const uint8_t *row = f->data + (size_t)y * f->source_stride;
    if (f->format == SOLAR_OS_DISPLAY_FORMAT_RGB565) return rgb_read(row + x * 2U);
    if (f->format == SOLAR_OS_DISPLAY_FORMAT_MONO1)
        return (((row[x / 8U] >> (x & 7U)) & 1U) != f->palette_inverted) ? 0 : 0xffff;
    unsigned index = f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? row[x] :
        (row[x / 4U] >> ((x & 3U) * 2U)) & 3U;
    if (f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 && f->palette_inverted) index = 3U - index;
    uint16_t color = f->palette_rgb565[index];
    return f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 && f->palette_inverted ? (uint16_t)~color : color;
}

static void base_set(solar_os_display_layer_t *l, unsigned x, unsigned y, uint16_t c)
{
    if (l->color) rgb_write(l->rgb + ((size_t)y * l->width + x) * 2U, c);
    unsigned luminance = ((c >> 11U) * 77U * 255U / 31U +
        ((c >> 5U) & 63U) * 150U * 255U / 63U + (c & 31U) * 29U * 255U / 31U) / 256U;
    canvas_set(l, l->base, x, y, luminance < 128U);
}

void solar_os_display_layer_raster(solar_os_display_layer_t *l, solar_os_display_raster_t *out)
{
    const size_t stride = l->color ? (size_t)l->width * 2U : (l->width + 7U) / 8U;
    if (l->color) memcpy(l->raster, l->rgb, stride * l->height);
    else memset(l->raster, 0, stride * l->height);
    for (unsigned y = 0; y < l->height; y++) {
        for (unsigned x = 0; x < l->width; x++) {
            const bool inside = x >= l->x && x < l->x + l->w && y >= l->y && y < l->y + l->h;
            if (l->color) {
                if (inside) rgb_write(l->raster + (size_t)y * stride + x * 2U,
                    l->colors[layer_black(l, x, y)]);
            } else if (inside ? layer_black(l, x, y) : canvas_black(l, x, y))
                l->raster[(size_t)y * stride + x / 8U] |= (uint8_t)(1U << (x & 7U));
        }
    }
    *out = (solar_os_display_raster_t){.data=l->raster, .data_size=stride*l->height,
        .source_width=l->width, .source_height=l->height, .source_stride=stride,
        .width=l->width, .height=l->height,
        .format=l->color ? SOLAR_OS_DISPLAY_FORMAT_RGB565 : SOLAR_OS_DISPLAY_FORMAT_MONO1};
}

void solar_os_display_layer_frame(solar_os_display_layer_t *l,
    const solar_os_display_raster_t *f, solar_os_display_raster_t *out)
{
    l->base_is_canvas = false;
    if (f->clear_background) {
        uint16_t color = f->background_rgb565;
        if (f->format != SOLAR_OS_DISPLAY_FORMAT_RGB565) {
            if (f->format == SOLAR_OS_DISPLAY_FORMAT_MONO1)
                color = ((f->background_index != 0) != f->palette_inverted) ? 0 : 0xffff;
            else {
                unsigned index = f->background_index;
                if (f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 && f->palette_inverted) index = 3U - index;
                color = f->palette_rgb565[index];
                if (f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 && f->palette_inverted) color = (uint16_t)~color;
            }
        }
        for (unsigned y = 0; y < l->height; y++)
            for (unsigned x = 0; x < l->width; x++) base_set(l, x, y, color);
    }
    for (unsigned y = 0; y < f->height; y++)
        for (unsigned x = 0; x < f->width; x++)
            base_set(l, f->x + x, f->y + y, frame_color(f,
                (uint32_t)x*f->source_width/f->width, (uint32_t)y*f->source_height/f->height));
    solar_os_display_layer_raster(l, out);
}

void solar_os_display_layer_surface(solar_os_display_layer_t *l,
    const solar_os_display_surface_t *s, solar_os_display_raster_t *out)
{
    l->base_is_canvas = false;
    for (unsigned y = 0; y < l->height; y++) {
        for (unsigned x = 0; x < l->width; x++) {
            base_set(l, x, y, s->palette_rgb565[s->data[(size_t)y*s->stride + x]]);
        }
    }
    solar_os_display_layer_raster(l, out);
}

void solar_os_display_layer_area(solar_os_display_layer_t *l,
    uint8_t *x, uint8_t *y, uint8_t *w, uint8_t *h)
{
    unsigned nx[2], ny[2];
    native_pixel(l, l->x, l->y, &nx[0], &ny[0]);
    native_pixel(l, l->x+l->w-1U, l->y+l->h-1U, &nx[1], &ny[1]);
    unsigned x0 = nx[0] < nx[1] ? nx[0] : nx[1], x1 = nx[0] > nx[1] ? nx[0] : nx[1];
    unsigned y0 = ny[0] < ny[1] ? ny[0] : ny[1], y1 = ny[0] > ny[1] ? ny[0] : ny[1];
    *x = x0/8U; *y = y0/8U; *w = x1/8U - *x + 1U; *h = y1/8U - *y + 1U;
}

void solar_os_display_canvas_pack(u8g2_t *canvas, uint8_t *bits,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    solar_os_display_layer_t l={.canvas=canvas, .base=u8g2_GetBufferPtr(canvas),
        .stride=(size_t)u8g2_GetBufferTileWidth(canvas)*8U, .black_is_one=true};
    size_t stride=(width+7U)/8U;
    memset(bits,0,stride*height);
    for (unsigned yy=0;yy<height;yy++)
        for (unsigned xx=0;xx<width;xx++)
            if (canvas_black(&l,x+xx,y+yy)) bits[yy*stride+xx/8U]|=(uint8_t)(1U<<(xx&7U));
}
