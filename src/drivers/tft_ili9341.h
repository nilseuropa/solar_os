#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "solar_os_display_surface.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *spi_bus;
    int cs_pin;
    int dc_pin;
    int reset_pin;
    int backlight_pin;
    uint32_t spi_clock_hz;
    uint32_t backlight_pwm_hz;
    uint16_t width;
    uint16_t height;
    uint8_t madctl;
    bool st7796;
    bool backlight_active_high;
    bool backlight_pwm;
    const u8g2_cb_t *rotation;
} tft_ili9341_config_t;

typedef struct {
    spi_device_handle_t spi;
    u8g2_t u8g2;
    uint8_t *buffer;
    uint8_t *shadow;
    uint8_t *line_buffer;
    uint8_t *line_buffer_alt;
    size_t buffer_size;
    size_t shadow_size;
    size_t line_buffer_size;
    const uint8_t *indexed_surface_data;
    uint64_t shadow_valid_rows;
    uint16_t foreground_rgb565;
    uint16_t background_rgb565;
    esp_err_t last_error;
    uint8_t backlight_percent;
    bool indexed_surface_valid;
    bool backlight_power;
    tft_ili9341_config_t config;
    u8x8_display_info_t display_info;
    uint16_t tile_width;
    uint16_t tile_height;
    size_t buffer_row_bytes;
} tft_ili9341_t;

esp_err_t tft_ili9341_init(tft_ili9341_t *display,
                           const tft_ili9341_config_t *config);
esp_err_t tft_ili9341_resume(tft_ili9341_t *display);
void tft_ili9341_deinit(tft_ili9341_t *display);
u8g2_t *tft_ili9341_get_u8g2(tft_ili9341_t *display);
bool tft_ili9341_backlight_supported(void);
esp_err_t tft_ili9341_get_backlight(const tft_ili9341_t *display, uint8_t *percent);
esp_err_t tft_ili9341_set_backlight(tft_ili9341_t *display, uint8_t percent);
esp_err_t tft_ili9341_set_colors(tft_ili9341_t *display,
                                 uint32_t foreground_rgb888,
                                 uint32_t background_rgb888);
esp_err_t tft_ili9341_present_surface(
    tft_ili9341_t *display,
    const solar_os_display_surface_t *surface);
esp_err_t tft_ili9341_present_frame(
    tft_ili9341_t *display,
    const solar_os_display_raster_t *frame);

#ifdef __cplusplus
}
#endif
