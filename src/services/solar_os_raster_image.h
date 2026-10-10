#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "solar_os_image_types_v1.h"
typedef struct solar_os_gfx solar_os_gfx_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Load one static raster frame from PNG, JPEG, GIF, or WebP storage. */
esp_err_t solar_os_raster_image_open(const char *path,
                                     solar_os_raster_image_t **out_image);
/* Decode borrowed compressed bytes synchronously; no reference to the input
 * survives this call. Suitable for leased camera/RTSP frames and HTTP bodies. */
esp_err_t solar_os_raster_image_decode(const uint8_t *data, size_t length,
                                       solar_os_raster_image_t **out_image);
/* Copy native pixels into a new immutable RGB888 image. The input may use
 * GRAY8, little-endian RGB565, or RGB888; stride zero means packed rows. */
esp_err_t solar_os_raster_image_from_pixels(const uint8_t *data, size_t length,
    uint32_t width, uint32_t height, solar_os_raster_image_format_t format,
    size_t stride, solar_os_raster_image_t **out_image);

/* References permit a script to close a handle while its draw is queued. */
void solar_os_raster_image_retain(solar_os_raster_image_t *image);
void solar_os_raster_image_release(solar_os_raster_image_t *image);

uint32_t solar_os_raster_image_width(const solar_os_raster_image_t *image);
uint32_t solar_os_raster_image_height(const solar_os_raster_image_t *image);

/* Borrow immutable RGB888 pixels. Keep an image reference for the entire use;
 * this view does not retain the image and is never an interpreter buffer. */
esp_err_t solar_os_raster_image_pixels(const solar_os_raster_image_t *image,
                                      solar_os_raster_image_pixels_t *pixels);
/* Copy/convert a crop into caller-owned storage using nearest-neighbour resize.
 * The destination must not overlap image pixels. Stride zero selects packed
 * rows. No allocation or modification of the source image occurs. */
esp_err_t solar_os_raster_image_convert(const solar_os_raster_image_t *image,
    const solar_os_raster_image_convert_options_t *options,
    uint8_t *destination, size_t length, size_t stride);

/* Draw with nearest-neighbour scaling and display clipping. A zero width or
 * height selects the source dimension for that axis. */
esp_err_t solar_os_raster_image_draw(const solar_os_raster_image_t *image,
                                     solar_os_gfx_t *gfx,
                                     int x,
                                     int y,
                                     uint32_t width,
                                     uint32_t height);
/* Present directly on RGB565-capable targets; otherwise draw/present through
 * the canvas. Rectangle must fit the target. Direct pixels are not retained in
 * the indexed canvas; later canvas presents may overwrite them. */
esp_err_t solar_os_raster_image_present(solar_os_raster_image_t *image,
    solar_os_gfx_t *gfx, int x, int y, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif
