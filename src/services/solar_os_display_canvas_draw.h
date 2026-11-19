#pragma once
#include "solar_os_display_geometry.h"

/* Shared terminal drawing keeps its rectangular coordinates; only the final
 * primitive is translated into the existing panel framebuffer. */
static inline u8g2_uint_t solar_os_canvas_x(u8g2_t *u, u8g2_uint_t x)
{ return x + solar_os_display_canvas_rect(u).x; }
static inline u8g2_uint_t solar_os_canvas_y(u8g2_t *u, u8g2_uint_t y)
{ return y + solar_os_display_canvas_rect(u).y; }

#define SOLAR_OS_CANVAS_RECT(name, draw) \
static inline void name(u8g2_t *u, u8g2_uint_t x, u8g2_uint_t y, u8g2_uint_t w, u8g2_uint_t h) \
{ draw(u, solar_os_canvas_x(u,x), solar_os_canvas_y(u,y), w, h); }
SOLAR_OS_CANVAS_RECT(solar_os_canvas_box, u8g2_DrawBox)
SOLAR_OS_CANVAS_RECT(solar_os_canvas_frame, u8g2_DrawFrame)
#undef SOLAR_OS_CANVAS_RECT

#define SOLAR_OS_CANVAS_LINE(name, draw) \
static inline void name(u8g2_t *u, u8g2_uint_t x, u8g2_uint_t y, u8g2_uint_t len) \
{ draw(u, solar_os_canvas_x(u,x), solar_os_canvas_y(u,y), len); }
SOLAR_OS_CANVAS_LINE(solar_os_canvas_hline, u8g2_DrawHLine)
SOLAR_OS_CANVAS_LINE(solar_os_canvas_vline, u8g2_DrawVLine)
#undef SOLAR_OS_CANVAS_LINE

static inline void solar_os_canvas_pixel(u8g2_t *u, u8g2_uint_t x, u8g2_uint_t y)
{ u8g2_DrawPixel(u, solar_os_canvas_x(u,x), solar_os_canvas_y(u,y)); }
static inline void solar_os_canvas_str(u8g2_t *u, u8g2_uint_t x, u8g2_uint_t y, const char *s)
{ u8g2_DrawStr(u, solar_os_canvas_x(u,x), solar_os_canvas_y(u,y), s); }
static inline void solar_os_canvas_glyph(u8g2_t *u, u8g2_uint_t x, u8g2_uint_t y, uint16_t c)
{ u8g2_DrawGlyph(u, solar_os_canvas_x(u,x), solar_os_canvas_y(u,y), c); }
static inline void solar_os_canvas_glyph_x2(u8g2_t *u, u8g2_uint_t x, u8g2_uint_t y, uint16_t c)
{ u8g2_DrawGlyphX2(u, solar_os_canvas_x(u,x), solar_os_canvas_y(u,y), c); }
