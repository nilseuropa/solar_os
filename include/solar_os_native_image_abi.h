#pragma once
#include "solar_os_native_service_abi.h"
#include "solar_os_image_types_v1.h"
#define SOLAR_OS_NATIVE_IMAGE_SERVICE "image"
#define SOLAR_OS_NATIVE_IMAGE_ABI 1U

/* Host-owned opaque images. open/decode/from_pixels return one reference.
 * Pixels are borrowed immutable RGB888 until the last reference is released.
 * Input bytes are borrowed only during synchronous creation/conversion. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*open)(const char *resolved_path, solar_os_raster_image_t **image);
    int (*decode)(const uint8_t *data, size_t length, solar_os_raster_image_t **image);
    int (*from_pixels)(const uint8_t *data, size_t length, uint32_t width,
        uint32_t height, solar_os_raster_image_format_t format, size_t stride,
        solar_os_raster_image_t **image);
    void (*retain)(solar_os_raster_image_t *image);
    void (*release)(solar_os_raster_image_t *image);
    int (*pixels)(const solar_os_raster_image_t *image, solar_os_raster_image_pixels_t *pixels);
    int (*convert)(const solar_os_raster_image_t *image,
        const solar_os_raster_image_convert_options_t *options,
        uint8_t *destination, size_t length, size_t stride);
} solar_os_native_image_api_v1_t;
