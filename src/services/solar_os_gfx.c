#include "solar_os_gfx_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t *gfx_font_data(solar_os_gfx_font_t font)
{
    switch (font) {
    case SOLAR_OS_GFX_FONT_SMALL:
        return u8g2_font_6x13_tf;
    case SOLAR_OS_GFX_FONT_BOLD:
        return u8g2_font_7x13B_tf;
    case SOLAR_OS_GFX_FONT_MONO:
    default:
        return u8g2_font_7x13_tf;
    }
}

static bool gfx_color_is_gray(solar_os_gfx_color_t color)
{
    const unsigned value = (unsigned)color;
    return value >= SOLAR_OS_GFX_COLOR_GRAY_BASE && value <= SOLAR_OS_GFX_COLOR_GRAY_LAST;
}

static uint8_t gfx_dither_threshold(solar_os_gfx_color_t color)
{
    if (gfx_color_is_gray(color)) {
        return (uint8_t)((unsigned)color - SOLAR_OS_GFX_COLOR_GRAY_BASE);
    }

    switch (color) {
    case SOLAR_OS_GFX_COLOR_WHITE:
        return 16;
    case SOLAR_OS_GFX_COLOR_LIGHT:
        return 12;
    case SOLAR_OS_GFX_COLOR_DARK:
        return 5;
    case SOLAR_OS_GFX_COLOR_BLACK:
    default:
        return 0;
    }
}

static uint8_t gfx_binary_draw_color(solar_os_gfx_color_t color)
{
    return gfx_dither_threshold(color) >= 8 ? 1 : 0;
}

static uint8_t gfx_pattern_draw_color(solar_os_gfx_color_t color, int x, int y)
{
    static const uint8_t bayer4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };

    const uint8_t threshold = gfx_dither_threshold(color);
    if (threshold == 0) {
        return 0;
    }
    if (threshold >= 16) {
        return 1;
    }

    return bayer4[y & 3][x & 3] < threshold ? 1 : 0;
}

static bool gfx_ready(const solar_os_gfx_t *gfx)
{
    return gfx != NULL && gfx->u8g2 != NULL;
}

static bool gfx_valid_rect(int width, int height)
{
    return width > 0 && height > 0;
}

static void gfx_sort_ints(int *values, size_t count)
{
    for (size_t i = 1; i < count; i++) {
        const int value = values[i];
        size_t j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = value;
    }
}

static void gfx_apply_draw_state(solar_os_gfx_t *gfx)
{
    if (!gfx_ready(gfx)) {
        return;
    }

    u8g2_SetDrawColor(gfx->u8g2, gfx_binary_draw_color(gfx->color));
    u8g2_SetFont(gfx->u8g2, gfx_font_data(gfx->font));
    u8g2_SetFontMode(gfx->u8g2, 1);
    u8g2_SetFontPosBaseline(gfx->u8g2);
}

static void gfx_mark_dirty(solar_os_gfx_t *gfx)
{
    if (gfx != NULL) {
        gfx->dirty = true;
    }
}

static void gfx_draw_hline_raw_clipped(solar_os_gfx_t *gfx, int x, int y, int width);

static void gfx_draw_hline_shade_clipped(solar_os_gfx_t *gfx, int x, int y, int width)
{
    if (!gfx_ready(gfx)) {
        return;
    }

    gfx_draw_hline_raw_clipped(gfx, x, y, width);
}

static void gfx_draw_hline_raw_clipped(solar_os_gfx_t *gfx, int x, int y, int width)
{
    if (!gfx_ready(gfx) || width <= 0) {
        return;
    }

    const int display_width = (int)u8g2_GetDisplayWidth(gfx->u8g2);
    const int display_height = (int)u8g2_GetDisplayHeight(gfx->u8g2);
    if (y < 0 || y >= display_height) {
        return;
    }

    int start = x;
    int end = x + width;
    if (end <= 0 || start >= display_width) {
        return;
    }
    if (start < 0) {
        start = 0;
    }
    if (end > display_width) {
        end = display_width;
    }
    if (end <= start) {
        return;
    }

    const uint8_t threshold = gfx_dither_threshold(gfx->color);
    if (threshold == 0 || threshold >= 16) {
        u8g2_SetDrawColor(gfx->u8g2, threshold >= 16 ? 1 : 0);
        u8g2_DrawHLine(gfx->u8g2,
                       (u8g2_uint_t)start,
                       (u8g2_uint_t)y,
                       (u8g2_uint_t)(end - start));
        return;
    }

    int run_start = start;
    uint8_t run_color = gfx_pattern_draw_color(gfx->color, start, y);
    for (int col = start + 1; col < end; col++) {
        const uint8_t color = gfx_pattern_draw_color(gfx->color, col, y);
        if (color == run_color) {
            continue;
        }

        u8g2_SetDrawColor(gfx->u8g2, run_color);
        u8g2_DrawHLine(gfx->u8g2,
                       (u8g2_uint_t)run_start,
                       (u8g2_uint_t)y,
                       (u8g2_uint_t)(col - run_start));
        run_start = col;
        run_color = color;
    }

    u8g2_SetDrawColor(gfx->u8g2, run_color);
    u8g2_DrawHLine(gfx->u8g2,
                   (u8g2_uint_t)run_start,
                   (u8g2_uint_t)y,
                   (u8g2_uint_t)(end - run_start));
}

void solar_os_gfx_init(solar_os_gfx_t *gfx, u8g2_t *u8g2)
{
    if (gfx == NULL) {
        return;
    }

    memset(gfx, 0, sizeof(*gfx));
    gfx->u8g2 = u8g2;
    gfx->color = SOLAR_OS_GFX_COLOR_BLACK;
    gfx->font = SOLAR_OS_GFX_FONT_MONO;
    gfx->line_style = SOLAR_OS_GFX_LINE_SOLID;
}

size_t solar_os_gfx_width(const solar_os_gfx_t *gfx)
{
    return gfx_ready(gfx) ? u8g2_GetDisplayWidth(gfx->u8g2) : 0;
}

size_t solar_os_gfx_height(const solar_os_gfx_t *gfx)
{
    return gfx_ready(gfx) ? u8g2_GetDisplayHeight(gfx->u8g2) : 0;
}

solar_os_gfx_color_t solar_os_gfx_gray(uint8_t level)
{
    if (level > SOLAR_OS_GFX_GRAY_MAX) {
        level = SOLAR_OS_GFX_GRAY_MAX;
    }

    return (solar_os_gfx_color_t)(SOLAR_OS_GFX_COLOR_GRAY_BASE + level);
}

bool solar_os_gfx_color_is_valid(solar_os_gfx_color_t color)
{
    if (gfx_color_is_gray(color)) {
        return true;
    }

    return color == SOLAR_OS_GFX_COLOR_WHITE ||
        color == SOLAR_OS_GFX_COLOR_LIGHT ||
        color == SOLAR_OS_GFX_COLOR_DARK ||
        color == SOLAR_OS_GFX_COLOR_BLACK;
}

void solar_os_gfx_set_color(solar_os_gfx_t *gfx, solar_os_gfx_color_t color)
{
    if (gfx == NULL) {
        return;
    }

    gfx->color = color;
}

solar_os_gfx_color_t solar_os_gfx_color(const solar_os_gfx_t *gfx)
{
    return gfx != NULL ? gfx->color : SOLAR_OS_GFX_COLOR_BLACK;
}

void solar_os_gfx_set_font(solar_os_gfx_t *gfx, solar_os_gfx_font_t font)
{
    if (gfx == NULL) {
        return;
    }

    gfx->font = font;
}

solar_os_gfx_font_t solar_os_gfx_font(const solar_os_gfx_t *gfx)
{
    return gfx != NULL ? gfx->font : SOLAR_OS_GFX_FONT_MONO;
}

void solar_os_gfx_set_line_style(solar_os_gfx_t *gfx, solar_os_gfx_line_style_t style)
{
    if (gfx == NULL) {
        return;
    }

    switch (style) {
    case SOLAR_OS_GFX_LINE_SOLID:
    case SOLAR_OS_GFX_LINE_DOTTED:
    case SOLAR_OS_GFX_LINE_DASHED:
        gfx->line_style = style;
        break;
    default:
        break;
    }
}

solar_os_gfx_line_style_t solar_os_gfx_line_style(const solar_os_gfx_t *gfx)
{
    return gfx != NULL ? gfx->line_style : SOLAR_OS_GFX_LINE_SOLID;
}

void solar_os_gfx_clear(solar_os_gfx_t *gfx, solar_os_gfx_color_t color)
{
    if (!gfx_ready(gfx)) {
        return;
    }

    const solar_os_gfx_color_t previous_color = gfx->color;
    gfx->color = color;
    for (int y = 0; y < (int)u8g2_GetDisplayHeight(gfx->u8g2); y++) {
        gfx_draw_hline_shade_clipped(gfx, 0, y, (int)u8g2_GetDisplayWidth(gfx->u8g2));
    }
    gfx->color = previous_color;
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_pixel(solar_os_gfx_t *gfx, int x, int y)
{
    if (!gfx_ready(gfx)) {
        return;
    }

    gfx_draw_hline_shade_clipped(gfx, x, y, 1);
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_line(solar_os_gfx_t *gfx, int x0, int y0, int x1, int y1)
{
    if (!gfx_ready(gfx)) {
        return;
    }

    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    unsigned step = 0;

    while (true) {
        bool draw = true;
        switch (gfx->line_style) {
        case SOLAR_OS_GFX_LINE_DOTTED:
            draw = (step % 4U) == 0U;
            break;
        case SOLAR_OS_GFX_LINE_DASHED:
            draw = (step % 12U) < 7U;
            break;
        case SOLAR_OS_GFX_LINE_SOLID:
        default:
            draw = true;
            break;
        }
        if (draw) {
            gfx_draw_hline_shade_clipped(gfx, x0, y0, 1);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        step++;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_rect(solar_os_gfx_t *gfx, int x, int y, int width, int height)
{
    if (!gfx_ready(gfx) || !gfx_valid_rect(width, height)) {
        return;
    }

    gfx_apply_draw_state(gfx);
    u8g2_DrawFrame(gfx->u8g2,
                   (u8g2_uint_t)x,
                   (u8g2_uint_t)y,
                   (u8g2_uint_t)width,
                   (u8g2_uint_t)height);
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_fill_rect(solar_os_gfx_t *gfx, int x, int y, int width, int height)
{
    if (!gfx_ready(gfx) || !gfx_valid_rect(width, height)) {
        return;
    }

    for (int row = y; row < y + height; row++) {
        gfx_draw_hline_shade_clipped(gfx, x, row, width);
    }
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_fill_polygon(solar_os_gfx_t *gfx,
                               const solar_os_gfx_point_t *points,
                               size_t point_count)
{
    if (!gfx_ready(gfx) || points == NULL || point_count < 3 || point_count > 16) {
        return;
    }

    int min_y = points[0].y;
    int max_y = points[0].y;
    for (size_t i = 1; i < point_count; i++) {
        if (points[i].y < min_y) {
            min_y = points[i].y;
        }
        if (points[i].y > max_y) {
            max_y = points[i].y;
        }
    }

    gfx_apply_draw_state(gfx);
    for (int y = min_y; y <= max_y; y++) {
        int intersections[16];
        size_t intersection_count = 0;
        for (size_t i = 0; i < point_count; i++) {
            const solar_os_gfx_point_t p0 = points[i];
            const solar_os_gfx_point_t p1 = points[(i + 1) % point_count];
            if (p0.y == p1.y) {
                continue;
            }
            if ((y < p0.y && y < p1.y) || (y >= p0.y && y >= p1.y)) {
                continue;
            }

            const int x = p0.x + ((y - p0.y) * (p1.x - p0.x)) / (p1.y - p0.y);
            intersections[intersection_count++] = x;
        }

        if (intersection_count < 2) {
            continue;
        }
        gfx_sort_ints(intersections, intersection_count);
        for (size_t i = 0; i + 1 < intersection_count; i += 2) {
            gfx_draw_hline_shade_clipped(gfx,
                                         intersections[i],
                                         y,
                                         intersections[i + 1] - intersections[i] + 1);
        }
    }
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_circle(solar_os_gfx_t *gfx, int x, int y, int radius)
{
    if (!gfx_ready(gfx) || radius <= 0) {
        return;
    }

    int dx = radius;
    int dy = 0;
    int err = 0;

    while (dx >= dy) {
        gfx_draw_hline_shade_clipped(gfx, x + dx, y + dy, 1);
        gfx_draw_hline_shade_clipped(gfx, x + dy, y + dx, 1);
        gfx_draw_hline_shade_clipped(gfx, x - dy, y + dx, 1);
        gfx_draw_hline_shade_clipped(gfx, x - dx, y + dy, 1);
        gfx_draw_hline_shade_clipped(gfx, x - dx, y - dy, 1);
        gfx_draw_hline_shade_clipped(gfx, x - dy, y - dx, 1);
        gfx_draw_hline_shade_clipped(gfx, x + dy, y - dx, 1);
        gfx_draw_hline_shade_clipped(gfx, x + dx, y - dy, 1);

        dy++;
        if (err <= 0) {
            err += (2 * dy) + 1;
        }
        if (err > 0) {
            dx--;
            err -= (2 * dx) + 1;
        }
    }
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_fill_circle(solar_os_gfx_t *gfx, int x, int y, int radius)
{
    if (!gfx_ready(gfx) || radius <= 0) {
        return;
    }

    gfx_apply_draw_state(gfx);
    const int radius_sq = radius * radius;
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = 0;
        while (((dx + 1) * (dx + 1)) + (dy * dy) <= radius_sq) {
            dx++;
        }
        gfx_draw_hline_shade_clipped(gfx, x - dx, y + dy, (dx * 2) + 1);
    }
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_text(solar_os_gfx_t *gfx, int x, int baseline_y, const char *text)
{
    if (!gfx_ready(gfx) || text == NULL) {
        return;
    }

    gfx_apply_draw_state(gfx);
    u8g2_DrawUTF8(gfx->u8g2, (u8g2_uint_t)x, (u8g2_uint_t)baseline_y, text);
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_bitmap(solar_os_gfx_t *gfx,
                         int x,
                         int y,
                         int width,
                         int height,
                         const uint8_t *bitmap)
{
    if (!gfx_ready(gfx) || !gfx_valid_rect(width, height) || bitmap == NULL) {
        return;
    }

    gfx_apply_draw_state(gfx);
    u8g2_DrawXBM(gfx->u8g2,
                 (u8g2_uint_t)x,
                 (u8g2_uint_t)y,
                 (u8g2_uint_t)width,
                 (u8g2_uint_t)height,
                 bitmap);
    gfx_mark_dirty(gfx);
}

bool solar_os_gfx_needs_present(const solar_os_gfx_t *gfx)
{
    return gfx != NULL && gfx->dirty;
}

void solar_os_gfx_present(solar_os_gfx_t *gfx)
{
    if (!gfx_ready(gfx)) {
        return;
    }

    u8g2_SendBuffer(gfx->u8g2);
    gfx->dirty = false;
}
