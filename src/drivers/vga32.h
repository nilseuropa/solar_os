#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_rom_lldesc.h"
#include "freertos/FreeRTOS.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VGA32_WIDTH 320U
#define VGA32_HEIGHT 200U
#define VGA32_DMA_DESCRIPTOR_COUNT 8U

typedef struct {
    u8g2_t u8g2;
    uint8_t *draw_buffer;
    uint8_t *scanout_buffers[2];
    uint8_t *dma_buffer;
    size_t draw_buffer_size;
    size_t scanout_buffer_size;
    size_t dma_buffer_size;
    lldesc_t dma_desc[VGA32_DMA_DESCRIPTOR_COUNT];
    intr_handle_t interrupt;
    portMUX_TYPE buffer_lock;
    volatile uint16_t last_eof_scanline;
    volatile uint8_t last_eof_descriptor;
    volatile int8_t current_buffer;
    volatile int8_t pending_buffer;
    volatile int8_t copying_buffer;
    volatile uint8_t foreground;
    volatile uint8_t background;
    esp_err_t last_error;
    bool signal_started;
} vga32_t;

esp_err_t vga32_init(vga32_t *display);
esp_err_t vga32_resume(vga32_t *display);
void vga32_deinit(vga32_t *display);
u8g2_t *vga32_get_u8g2(vga32_t *display);
esp_err_t vga32_set_colors(vga32_t *display,
                           uint32_t foreground_rgb888,
                           uint32_t background_rgb888);
esp_err_t vga32_present_mono_xbm(vga32_t *display,
                                 const uint8_t *bitmap,
                                 size_t bitmap_size,
                                 uint16_t x,
                                 uint16_t y,
                                 uint16_t width,
                                 uint16_t height,
                                 uint16_t stride,
                                 bool palette_inverted);

#ifdef __cplusplus
}
#endif
