#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "solar_os_display_surface.h"
typedef struct solar_os_gfx solar_os_gfx_t;
size_t solar_os_gfx_width(const solar_os_gfx_t *gfx);
size_t solar_os_gfx_height(const solar_os_gfx_t *gfx);
bool solar_os_gfx_display_target_name(const solar_os_gfx_t *gfx, char *name, size_t size);
bool solar_os_gfx_supports_frame_format(const solar_os_gfx_t *gfx, solar_os_display_format_t format);
esp_err_t solar_os_gfx_present_frame(solar_os_gfx_t *gfx, const solar_os_display_raster_t *frame);
