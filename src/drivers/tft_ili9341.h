#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_device_handle_t spi;
    u8g2_t u8g2;
    uint8_t *buffer;
    uint8_t *shadow;
    uint8_t *line_buffer;
    size_t buffer_size;
    size_t shadow_size;
    size_t line_buffer_size;
    uint64_t shadow_valid_rows;
    uint16_t foreground_rgb565;
    uint16_t background_rgb565;
    esp_err_t last_error;
    uint8_t backlight_percent;
    bool bus_acquired;
    bool backlight_power;
} tft_ili9341_t;

esp_err_t tft_ili9341_init(tft_ili9341_t *display);
esp_err_t tft_ili9341_resume(tft_ili9341_t *display);
void tft_ili9341_deinit(tft_ili9341_t *display);
u8g2_t *tft_ili9341_get_u8g2(tft_ili9341_t *display);
bool tft_ili9341_backlight_supported(void);
esp_err_t tft_ili9341_get_backlight(const tft_ili9341_t *display, uint8_t *percent);
esp_err_t tft_ili9341_set_backlight(tft_ili9341_t *display, uint8_t percent);
esp_err_t tft_ili9341_set_colors(tft_ili9341_t *display,
                                 uint32_t foreground_rgb888,
                                 uint32_t background_rgb888);

#ifdef __cplusplus
}
#endif
