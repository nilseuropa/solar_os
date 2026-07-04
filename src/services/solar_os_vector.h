#pragma once

#include <stddef.h>
#include <stdint.h>

void solar_os_vector_fill_rgb565_be(uint8_t *dst, uint16_t rgb565, size_t pixels);
void solar_os_vector_expand_1bpp_to_rgb565_be(uint8_t *dst,
                                              const uint8_t *columns,
                                              unsigned bit,
                                              uint16_t zero_rgb565,
                                              uint16_t one_rgb565,
                                              size_t pixels);
void solar_os_vector_rgba_to_gray_scaled(uint8_t *dst_gray,
                                         uint32_t dst_width,
                                         uint32_t dst_height,
                                         const uint8_t *src_rgba,
                                         uint32_t src_width,
                                         uint32_t src_height);
