#include "solar_os_splash.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "solar_os_config.h"

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

#ifndef SOLAR_OS_FLAVOR_NAME
#define SOLAR_OS_FLAVOR_NAME "full"
#endif

static int splash_center_text_x(solar_os_gfx_t *gfx, int center_x, const char *text)
{
    const int text_width = (int)solar_os_gfx_text_width(gfx, text);
    const int x = center_x - (text_width / 2);
    if (x < 2) {
        return 2;
    }
    return x;
}

static int splash_min_int(int a, int b)
{
    return a < b ? a : b;
}

static void splash_draw_common(solar_os_gfx_t *gfx, const char *status, bool reboot)
{
    if (gfx == NULL) {
        return;
    }

    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    if (width <= 0 || height <= 0) {
        return;
    }

    const int center_x = width / 2;
    const int radius = splash_min_int(width, height) / 8;
    const int center_y = splash_min_int(height / 2 - 24, height - 110);
    const char *state = status != NULL && status[0] != '\0' ? status : "starting";
    const char *title = "SolarOS";
    char build[64];

    (void)reboot;

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_circle(gfx, center_x, center_y, radius);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
    solar_os_gfx_text(gfx,
                      splash_center_text_x(gfx, center_x, title),
                      center_y + radius + 34,
                      title);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_14);
    solar_os_gfx_text(gfx,
                      splash_center_text_x(gfx, center_x, state),
                      center_y + radius + 54,
                      state);

    snprintf(build, sizeof(build), "v%s %s", SOLAR_OS_VERSION, SOLAR_OS_FLAVOR_NAME);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    solar_os_gfx_text(gfx,
                      splash_center_text_x(gfx, center_x, build),
                      height - 10,
                      build);

    solar_os_gfx_present(gfx);
}

void solar_os_splash_clear(solar_os_gfx_t *gfx)
{
    if (gfx == NULL) {
        return;
    }
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_present(gfx);
}

void solar_os_splash_draw(solar_os_gfx_t *gfx, const char *status)
{
    splash_draw_common(gfx, status, false);
}

void solar_os_splash_draw_reboot(solar_os_gfx_t *gfx, const char *status)
{
    splash_draw_common(gfx, status != NULL ? status : "restarting", true);
}
