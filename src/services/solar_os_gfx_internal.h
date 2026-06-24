#pragma once

#include "solar_os_gfx.h"
#include "u8g2.h"

struct solar_os_gfx {
    u8g2_t *u8g2;
    solar_os_gfx_color_t color;
    solar_os_gfx_font_t font;
    solar_os_gfx_line_style_t line_style;
    bool dirty;
};

void solar_os_gfx_init(solar_os_gfx_t *gfx, u8g2_t *u8g2);
