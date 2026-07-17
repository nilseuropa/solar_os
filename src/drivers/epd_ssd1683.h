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

typedef enum {
    EPD_SSD1683_REFRESH_AUTO = 0,
    EPD_SSD1683_REFRESH_FAST,
    EPD_SSD1683_REFRESH_FULL,
} epd_ssd1683_refresh_mode_t;

typedef enum {
    EPD_SSD1683_PANEL_UNKNOWN = 0,
    EPD_SSD1683_PANEL_LEGACY,
    EPD_SSD1683_PANEL_GREEN_STICKER,
} epd_ssd1683_panel_variant_t;

typedef struct {
    spi_device_handle_t spi;
    u8g2_t u8g2;
    uint8_t *buffer;
    uint8_t *shadow;
    uint8_t *line_buffer;
    size_t buffer_size;
    size_t shadow_size;
    size_t line_buffer_size;
    esp_err_t last_error;
    epd_ssd1683_refresh_mode_t refresh_mode;
    epd_ssd1683_panel_variant_t panel_variant;
    uint8_t fast_refresh_count;
    uint8_t refresh_log_count;
    bool bus_initialized;
    bool controller_ready;
    bool shadow_valid;
    bool powered;
} epd_ssd1683_t;

esp_err_t epd_ssd1683_init(epd_ssd1683_t *display);
esp_err_t epd_ssd1683_resume(epd_ssd1683_t *display);
void epd_ssd1683_deinit(epd_ssd1683_t *display);
u8g2_t *epd_ssd1683_get_u8g2(epd_ssd1683_t *display);
const char *epd_ssd1683_controller_mode(const epd_ssd1683_t *display);
const char *epd_ssd1683_controller_mode_values(const epd_ssd1683_t *display);
esp_err_t epd_ssd1683_set_controller_mode(epd_ssd1683_t *display, const char *mode);

#ifdef __cplusplus
}
#endif
