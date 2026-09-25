#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct solar_os_gfx solar_os_gfx_t;
typedef struct solar_os_raster_image solar_os_raster_image_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Load one static raster frame from PNG, JPEG, GIF, or WebP storage. */
esp_err_t solar_os_raster_image_open(const char *path,
                                     solar_os_raster_image_t **out_image);

/* References permit a script to close a handle while its draw is queued. */
void solar_os_raster_image_retain(solar_os_raster_image_t *image);
void solar_os_raster_image_release(solar_os_raster_image_t *image);

uint32_t solar_os_raster_image_width(const solar_os_raster_image_t *image);
uint32_t solar_os_raster_image_height(const solar_os_raster_image_t *image);

/* Draw with nearest-neighbour scaling and display clipping. A zero width or
 * height selects the source dimension for that axis. */
esp_err_t solar_os_raster_image_draw(const solar_os_raster_image_t *image,
                                     solar_os_gfx_t *gfx,
                                     int x,
                                     int y,
                                     uint32_t width,
                                     uint32_t height);

#ifdef __cplusplus
}
#endif
