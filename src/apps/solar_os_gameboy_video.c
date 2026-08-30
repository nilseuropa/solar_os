#include "solar_os_gameboy_video.h"

#include <string.h>

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
