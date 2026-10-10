#pragma once
#include "solar_os_inference.h"
#include "solar_os_raster_image.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "solar_os_tensor_image_types_v1.h"

void solar_os_tensor_image_defaults(solar_os_tensor_image_options_t *options);
esp_err_t solar_os_tensor_image_option(solar_os_tensor_image_options_t *options,
    const char *key, const char *value);
/* Validate source/port/options and obtain exact allocation and inverse-map geometry.
 * Batch must be one; int8/uint8/int16/float32, nearest-neighbor stretch/letterbox.
 * The explicitly selected layout defines the axis for per-channel exponents. */
esp_err_t solar_os_tensor_image_inspect(const solar_os_raster_image_pixels_t *source,
    const solar_os_inference_tensor_t *port, const solar_os_tensor_image_options_t *options,
    solar_os_tensor_image_transform_t *transform);
/* Caller owns the destination. Source is immutable and must not overlap it.
 * Lookup storage uses PSRAM only, is temporary, and is freed on every return.
 * Cancellation may leave partial destination bytes; publish only on ESP_OK. */
esp_err_t solar_os_tensor_image_prepare(const solar_os_raster_image_pixels_t *source,
    const solar_os_inference_tensor_t *port, const solar_os_tensor_image_options_t *options,
    uint8_t *destination, size_t length, solar_os_tensor_image_transform_t *transform,
    solar_os_inference_cancel_fn cancel, void *user);
#ifdef __cplusplus
}
#endif
