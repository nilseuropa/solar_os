#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_rom_lldesc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SOLAR_OS_VGA_MODE_320X200
#define SOLAR_OS_VGA_MODE_320X200 0
#endif
#ifndef SOLAR_OS_VGA_MODE_320X240
#define SOLAR_OS_VGA_MODE_320X240 0
#endif
#ifndef SOLAR_OS_VGA_MODE_640X400
#define SOLAR_OS_VGA_MODE_640X400 0
#endif
#ifndef SOLAR_OS_VGA_MODE_640X480
#define SOLAR_OS_VGA_MODE_640X480 1
#endif

#if SOLAR_OS_VGA_MODE_320X200
#define VGA32_WIDTH 320U
#define VGA32_HEIGHT 200U
#elif SOLAR_OS_VGA_MODE_320X240
#define VGA32_WIDTH 320U
#define VGA32_HEIGHT 240U
#elif SOLAR_OS_VGA_MODE_640X400
#define VGA32_WIDTH 640U
#define VGA32_HEIGHT 400U
#else
#define VGA32_WIDTH 640U
#define VGA32_HEIGHT 480U
#endif
#define VGA32_DMA_DESCRIPTOR_COUNT 8U
#define VGA32_PRESENT_BUFFER_COUNT 2U
#if SOLAR_OS_VGA_MODE_320X200 || SOLAR_OS_VGA_MODE_320X240
#define VGA32_SCANOUT_BUFFER_COUNT 2U
#else
#define VGA32_SCANOUT_BUFFER_COUNT 1U
#endif

typedef struct {
    int red0_pin;
    int red1_pin;
    int green0_pin;
    int green1_pin;
    int blue0_pin;
    int blue1_pin;
    int hsync_pin;
    int vsync_pin;
    const u8g2_cb_t *rotation;
} vga32_config_t;

typedef struct {
    u8g2_t u8g2;
    uint8_t *draw_buffer;
    uint8_t *present_buffers[VGA32_PRESENT_BUFFER_COUNT];
    uint8_t *scanout_buffers[VGA32_SCANOUT_BUFFER_COUNT];
    uint8_t *dma_buffer;
    size_t draw_buffer_size;
    size_t scanout_buffer_size;
    size_t dma_buffer_size;
    lldesc_t dma_desc[VGA32_DMA_DESCRIPTOR_COUNT];
    uint32_t pixel_lut[256][2];
    intr_handle_t interrupt;
    TaskHandle_t present_task;
    portMUX_TYPE buffer_lock;
    portMUX_TYPE present_lock;
    volatile uint16_t last_eof_scanline;
    volatile uint8_t last_eof_descriptor;
    volatile int8_t current_buffer;
    volatile int8_t pending_buffer;
    volatile int8_t copying_buffer;
    volatile int8_t pending_present_buffer;
    volatile int8_t rendering_present_buffer;
    volatile int8_t copying_present_buffer;
    volatile uint8_t foreground;
    volatile uint8_t background;
    volatile bool present_stop_requested;
    uint32_t present_queued_frames;
    uint32_t present_rendered_frames;
    uint32_t present_coalesced_frames;
    uint32_t present_max_copy_us;
    uint32_t present_max_render_us;
    esp_err_t last_error;
    bool signal_started;
    vga32_config_t config;
} vga32_t;

esp_err_t vga32_init(vga32_t *display, const vga32_config_t *config);
esp_err_t vga32_start_async_present(vga32_t *display);
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
