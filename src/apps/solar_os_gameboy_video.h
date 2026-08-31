#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_GAMEBOY_LCD_WIDTH 160U
#define SOLAR_OS_GAMEBOY_LCD_HEIGHT 144U
#define SOLAR_OS_GAMEBOY_BITMAP_WIDTH SOLAR_OS_GAMEBOY_LCD_WIDTH
#define SOLAR_OS_GAMEBOY_BITMAP_HEIGHT SOLAR_OS_GAMEBOY_LCD_HEIGHT
#define SOLAR_OS_GAMEBOY_BITMAP_STRIDE (SOLAR_OS_GAMEBOY_BITMAP_WIDTH / 4U)
#define SOLAR_OS_GAMEBOY_BITMAP_BYTES                                          \
  (SOLAR_OS_GAMEBOY_BITMAP_STRIDE * SOLAR_OS_GAMEBOY_BITMAP_HEIGHT)

void solar_os_gameboy_video_clear(uint8_t *bitmap, size_t bitmap_len);
bool solar_os_gameboy_video_scanline(uint8_t *bitmap, size_t bitmap_len,
                                     const uint8_t *pixels, size_t line);
void solar_os_gameboy_video_theme_palette(uint32_t foreground_rgb888,
                                          uint32_t background_rgb888,
                                          uint16_t palette_rgb565[4]);
