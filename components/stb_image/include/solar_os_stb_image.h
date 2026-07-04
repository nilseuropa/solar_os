#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t frame_count;
    uint8_t *gray;
    uint32_t *delays_ms;
} solar_os_stb_gif_animation_t;

typedef void (*solar_os_stb_rgba_to_gray_scaled_fn)(uint8_t *dst_gray,
                                                    uint32_t dst_width,
                                                    uint32_t dst_height,
                                                    const uint8_t *src_rgba,
                                                    uint32_t src_width,
                                                    uint32_t src_height);

esp_err_t solar_os_stb_decode_gray(const uint8_t *data,
                                   size_t len,
                                   uint32_t max_pixels,
                                   uint8_t **out_gray,
                                   uint32_t *out_width,
                                   uint32_t *out_height);
esp_err_t solar_os_stb_decode_gif_gray(const uint8_t *data,
                                       size_t len,
                                       uint32_t max_frame_pixels,
                                       uint32_t max_total_pixels,
                                       uint32_t max_output_width,
                                       uint32_t max_output_height,
                                       solar_os_stb_rgba_to_gray_scaled_fn converter,
                                       solar_os_stb_gif_animation_t *out_animation);
esp_err_t solar_os_stb_jpeg_decode_gray(const uint8_t *data,
                                         size_t len,
                                         uint32_t max_pixels,
                                         uint8_t **out_gray,
                                         uint32_t *out_width,
                                         uint32_t *out_height);
void solar_os_stb_gif_animation_free(solar_os_stb_gif_animation_t *animation);
void solar_os_stb_image_free(void *data);
const char *solar_os_stb_failure_reason(void);

#ifdef __cplusplus
}
#endif
