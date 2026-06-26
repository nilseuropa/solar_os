#pragma once

#include "solar_os_gfx.h"

void solar_os_splash_clear(solar_os_gfx_t *gfx);
void solar_os_splash_draw(solar_os_gfx_t *gfx, const char *status);
void solar_os_splash_draw_reboot(solar_os_gfx_t *gfx, const char *status);
