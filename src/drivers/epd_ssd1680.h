#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "u8g2.h"
#include "solar_os_epd.h"
#include "epd_relax.h"

/* Monochrome 2.13-inch V4-style glass with full/partial waveforms in OTP. */
#define EPD_SSD1680_PANEL_122X250 0U
#define EPD_SSD1680_WIDTH 122U
#define EPD_SSD1680_HEIGHT 250U
#define EPD_SSD1680_ROW_BYTES ((EPD_SSD1680_WIDTH + 7U) / 8U)
#define EPD_SSD1680_BUFFER_STRIDE (EPD_SSD1680_ROW_BYTES * 8U)
#define EPD_SSD1680_TILE_ROWS ((EPD_SSD1680_HEIGHT + 7U) / 8U)
#define EPD_SSD1680_BUFFER_BYTES (EPD_SSD1680_BUFFER_STRIDE * EPD_SSD1680_TILE_ROWS)

typedef struct {
    const char *spi_bus;
    int cs_pin, dc_pin, reset_pin, busy_pin, power_pin;
    int spi_clock_hz;
    const u8g2_cb_t *rotation;
    unsigned panel;
} epd_ssd1680_config_t;

typedef enum {
    EPD_SSD1680_REFRESH_AUTO,
    EPD_SSD1680_REFRESH_PARTIAL,
    EPD_SSD1680_REFRESH_FULL,
} epd_ssd1680_refresh_mode_t;

typedef struct {
    spi_device_handle_t spi;
    solar_os_epd_maintenance_t maintenance;
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t lock;
    u8g2_t u8g2;
    uint8_t *buffer, *shadow, *line_buffer;
    esp_err_t last_error;
    epd_ssd1680_refresh_mode_t refresh_mode;
    unsigned partial_count;
    epd_relax_t relax;
    int dc_pin, reset_pin, busy_pin, power_pin;
    bool controller_ready, shadow_valid, powered, pins_configured, suspended;
    char mode_description[256];
} epd_ssd1680_t;

esp_err_t epd_ssd1680_init(epd_ssd1680_t *display, const epd_ssd1680_config_t *config);
esp_err_t epd_ssd1680_refresh(epd_ssd1680_t *display);
esp_err_t epd_ssd1680_present_power_state(epd_ssd1680_t *display);
esp_err_t epd_ssd1680_resume(epd_ssd1680_t *display);
void epd_ssd1680_deinit(epd_ssd1680_t *display);
esp_err_t epd_ssd1680_relax(epd_ssd1680_t *display, solar_os_display_relax_phase_t phase);
/* Byte-padded, LSB-first MONO1; set bits are black. Coordinates follow rotation. */
esp_err_t epd_ssd1680_present_mono_xbm(epd_ssd1680_t *display,
    const uint8_t *bitmap, size_t bitmap_size, uint16_t x, uint16_t y,
    uint16_t width, uint16_t height, uint16_t stride, bool inverted);
u8g2_t *epd_ssd1680_get_u8g2(epd_ssd1680_t *display);
const char *epd_ssd1680_controller_mode(const epd_ssd1680_t *display);
const char *epd_ssd1680_controller_mode_values(const epd_ssd1680_t *display);
esp_err_t epd_ssd1680_set_controller_mode(epd_ssd1680_t *display, const char *mode);
