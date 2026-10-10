#pragma once
/* Version 1 native data contract. Layouts and enum values are frozen for this ABI.
 * This header has no ESP-IDF or interpreter dependency. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct solar_os_raster_image solar_os_raster_image_t;

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t stride;
    uint32_t width, height;
} solar_os_raster_image_pixels_t;

typedef enum {
    SOLAR_OS_RASTER_IMAGE_GRAY8,
    SOLAR_OS_RASTER_IMAGE_RGB565_LE,
    SOLAR_OS_RASTER_IMAGE_RGB888,
} solar_os_raster_image_format_t;

typedef struct {
    uint32_t x, y, width, height; /* Zero dimensions select the remaining image. */
    uint32_t output_width, output_height; /* Zero selects the crop dimension. */
    solar_os_raster_image_format_t format;
} solar_os_raster_image_convert_options_t;
