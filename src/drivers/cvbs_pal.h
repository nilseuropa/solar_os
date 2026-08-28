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

#ifndef SOLAR_OS_CVBS_MODE_320X200
#define SOLAR_OS_CVBS_MODE_320X200 0
#endif

#if SOLAR_OS_CVBS_MODE_320X200
#define CVBS_PAL_WIDTH 320U
#define CVBS_PAL_HEIGHT 200U
#define CVBS_DMA_DESCRIPTOR_COUNT 8U
#define CVBS_DMA_DYNAMIC_LINE_COUNT 8U
#else
#define CVBS_PAL_WIDTH 384U
#define CVBS_PAL_HEIGHT 288U
/* 575 visible lines plus 15 four-run and 35 two-run sync lines. */
#define CVBS_DMA_DESCRIPTOR_COUNT 705U
#define CVBS_DMA_DYNAMIC_LINE_COUNT 4U
#endif

typedef struct {
    int output_pin;
    const u8g2_cb_t *rotation;
} cvbs_pal_config_t;

typedef struct {
    u8g2_t u8g2;
    uint8_t *draw_buffer;
    uint8_t *scanout_buffers[2];
    uint8_t *dma_buffer;
    uint8_t *dma_static_buffer;
    size_t buffer_size;
    size_t dma_buffer_size;
    size_t dma_static_buffer_size;
    lldesc_t dma_desc[CVBS_DMA_DESCRIPTOR_COUNT];
#if !SOLAR_OS_CVBS_MODE_320X200
    uint16_t dma_refill_scanline[CVBS_DMA_DESCRIPTOR_COUNT];
#endif
    intr_handle_t interrupt;
    portMUX_TYPE buffer_lock;
    volatile uint16_t last_eof_scanline;
    volatile uint8_t last_eof_descriptor;
    volatile int8_t current_buffer;
    volatile int8_t pending_buffer;
    volatile int8_t copying_buffer;
    esp_err_t last_error;
    bool signal_started;
    cvbs_pal_config_t config;
} cvbs_pal_t;

esp_err_t cvbs_pal_init(cvbs_pal_t *display, const cvbs_pal_config_t *config);
esp_err_t cvbs_pal_resume(cvbs_pal_t *display);
esp_err_t cvbs_pal_present_mono_xbm(cvbs_pal_t *display,
                                    const uint8_t *bitmap,
                                    size_t bitmap_size,
                                    uint16_t x,
                                    uint16_t y,
                                    uint16_t width,
                                    uint16_t height,
                                    uint16_t stride,
                                    bool palette_inverted);
void cvbs_pal_deinit(cvbs_pal_t *display);
u8g2_t *cvbs_pal_get_u8g2(cvbs_pal_t *display);

#ifdef __cplusplus
}
#endif
