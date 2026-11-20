#include "solar_os_display_geometry.h"

static solar_os_display_rect_t default_canvas(const solar_os_display_geometry_t *g)
{
    if (!g->round) return (solar_os_display_rect_t){0, 0, g->native_width, g->native_height};
    unsigned diameter = g->native_width < g->native_height ? g->native_width : g->native_height;
    unsigned lo = 0, hi = diameter;
    while (lo < hi) {
        unsigned side = (lo + hi + 1U) / 2U;
        if (2ULL * side * side <= (uint64_t)diameter * diameter) lo = side;
        else hi = side - 1U;
    }
    if (!lo) lo = 1;
    return (solar_os_display_rect_t){(g->native_width - lo) / 2,
        (g->native_height - lo) / 2, lo, lo};
}

void solar_os_display_geometry_update(solar_os_display_geometry_t *g,
    uint16_t rotation, uint8_t scale)
{
    solar_os_display_rect_t c = g->native_canvas;
    const int w = g->native_width, h = g->native_height;
    if (rotation == 90) c = (solar_os_display_rect_t){h-c.y-c.height, c.x, c.height, c.width};
    else if (rotation == 180) c = (solar_os_display_rect_t){w-c.x-c.width, h-c.y-c.height, c.width, c.height};
    else if (rotation == 270) c = (solar_os_display_rect_t){c.y, w-c.x-c.width, c.height, c.width};
    g->rotation = rotation;
    g->scale = scale ? scale : 1;
    const bool quarter = rotation == 90 || rotation == 270;
    g->panel = (solar_os_display_rect_t){0, 0, (quarter ? h : w) / g->scale,
        (quarter ? w : h) / g->scale};
    c.x /= g->scale; c.y /= g->scale;
    c.width /= g->scale; c.height /= g->scale;
    if (!c.width) c.width = 1;
    if (!c.height) c.height = 1;
    if (c.x >= g->panel.width) c.x = g->panel.width-1;
    if (c.y >= g->panel.height) c.y = g->panel.height-1;
    if (c.width > g->panel.width-c.x) c.width = g->panel.width-c.x;
    if (c.height > g->panel.height-c.y) c.height = g->panel.height-c.y;
    g->canvas = c;
}

void solar_os_display_geometry_init(solar_os_display_geometry_t *g,
    uint16_t width, uint16_t height, bool round)
{
    *g = (solar_os_display_geometry_t){.native_width=width, .native_height=height, .round=round};
    g->native_canvas = default_canvas(g);
    solar_os_display_geometry_update(g, 0, 1);
}

bool solar_os_display_geometry_set_canvas(solar_os_display_geometry_t *g,
    const solar_os_display_rect_t *canvas)
{
    if (!canvas) g->native_canvas = default_canvas(g);
    else {
        solar_os_display_rect_t c = *canvas;
        if (c.x < 0 || c.y < 0 || c.width <= 0 || c.height <= 0 ||
            c.width > g->panel.width || c.height > g->panel.height ||
            c.x > g->panel.width-c.width || c.y > g->panel.height-c.height) return false;
        c.x *= g->scale; c.y *= g->scale;
        c.width *= g->scale; c.height *= g->scale;
        if (g->rotation == 90) c = (solar_os_display_rect_t){c.y, g->native_height-c.x-c.width, c.height, c.width};
        else if (g->rotation == 180) c = (solar_os_display_rect_t){g->native_width-c.x-c.width,
            g->native_height-c.y-c.height, c.width, c.height};
        else if (g->rotation == 270) c = (solar_os_display_rect_t){g->native_width-c.y-c.height, c.x, c.height, c.width};
        g->native_canvas = c;
    }
    solar_os_display_geometry_update(g, g->rotation, g->scale);
    return true;
}
