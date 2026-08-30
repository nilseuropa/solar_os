#include "solar_os_gameboy_video.h"

#include <string.h>

static uint8_t blend_thirds(uint8_t background, uint8_t foreground,
                            unsigned foreground_thirds) {
  return (uint8_t)(((unsigned)background * (3U - foreground_thirds) +
                    (unsigned)foreground * foreground_thirds + 1U) /
                   3U);
}

static uint16_t rgb888_to_rgb565(uint32_t rgb888) {
  return (uint16_t)(((rgb888 >> 8U) & 0xf800U) |
                    ((rgb888 >> 5U) & 0x07e0U) |
                    ((rgb888 >> 3U) & 0x001fU));
}

void solar_os_gameboy_video_theme_palette(uint32_t foreground_rgb888,
                                          uint32_t background_rgb888,
                                          uint16_t palette_rgb565[4]) {
  if (palette_rgb565 == NULL) {
    return;
  }
  foreground_rgb888 &= 0xffffffU;
  background_rgb888 &= 0xffffffU;
  for (unsigned shade = 0U; shade < 4U; shade++) {
    const uint8_t red = blend_thirds((uint8_t)(background_rgb888 >> 16U),
                                     (uint8_t)(foreground_rgb888 >> 16U),
                                     shade);
    const uint8_t green = blend_thirds((uint8_t)(background_rgb888 >> 8U),
                                       (uint8_t)(foreground_rgb888 >> 8U),
                                       shade);
    const uint8_t blue = blend_thirds((uint8_t)background_rgb888,
                                      (uint8_t)foreground_rgb888, shade);
    palette_rgb565[shade] = rgb888_to_rgb565(
        ((uint32_t)red << 16U) | ((uint32_t)green << 8U) | blue);
  }
}

void solar_os_gameboy_video_clear(uint8_t *bitmap, size_t bitmap_len) {
  if (bitmap == NULL || bitmap_len < SOLAR_OS_GAMEBOY_BITMAP_BYTES) {
    return;
  }
  memset(bitmap, 0, SOLAR_OS_GAMEBOY_BITMAP_BYTES);
}

bool solar_os_gameboy_video_scanline(uint8_t *bitmap, size_t bitmap_len,
                                     const uint8_t *pixels, size_t line) {
  if (bitmap == NULL || pixels == NULL ||
      bitmap_len < SOLAR_OS_GAMEBOY_BITMAP_BYTES ||
      line >= SOLAR_OS_GAMEBOY_LCD_HEIGHT) {
    return false;
  }

  uint8_t *output = bitmap + line * SOLAR_OS_GAMEBOY_BITMAP_STRIDE;
  for (size_t source_x = 0; source_x < SOLAR_OS_GAMEBOY_LCD_WIDTH; source_x++) {
    const uint8_t shade = pixels[source_x] & 0x03U;
    const size_t output_byte = source_x >> 2U;
    const unsigned shift = (unsigned)(source_x & 3U) * 2U;
    output[output_byte] = (uint8_t)(
        (output[output_byte] & (uint8_t)~(3U << shift)) | (shade << shift));
  }
  return true;
}
