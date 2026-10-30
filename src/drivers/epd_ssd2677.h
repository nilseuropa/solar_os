#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "u8g2.h"
#include "epd_relax.h"

#define EPD_SSD2677_PANEL_GENERIC 0U
#define EPD_SSD2677_PANEL_STICKY 1U

typedef enum {
    EPD_SSD2677_REFRESH_AUTO,
    EPD_SSD2677_REFRESH_PARTIAL,
    EPD_SSD2677_REFRESH_FULL,
} epd_ssd2677_refresh_mode_t;

typedef esp_err_t (*epd_ssd2677_power_fn_t)(void *context, bool on);

/* Both profile values select the Sticky 3D97 V2 monochrome panel. Other
 * SSD2677 glass needs its own initialization and waveform profile. */
typedef struct {
    const char *spi_bus;
    spi_host_device_t spi_host;
    int sclk_pin, mosi_pin, cs_pin, dc_pin, reset_pin, busy_pin, power_pin;
    int spi_clock_hz, busy_level, power_active_level;
    unsigned panel;
    const u8g2_cb_t *rotation;
    epd_ssd2677_power_fn_t set_power;
    void *power_context;
} epd_ssd2677_config_t;

typedef struct {
    spi_device_handle_t spi;
    spi_host_device_t spi_host;
    u8g2_t u8g2;
    uint8_t *buffer, *shadow, *line_buffer;
    size_t buffer_size, shadow_size, line_buffer_size;
    esp_err_t last_error;
    epd_ssd2677_refresh_mode_t refresh_mode;
    epd_relax_t relax;
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t lock;
    int dc_pin, reset_pin, busy_pin, power_pin, power_active_level;
    epd_ssd2677_power_fn_t set_power;
    void *power_context;
    unsigned partial_refresh_count;
    uint8_t temperature;
    bool bus_initialized, controller_ready, shadow_valid, powered, analog_on;
    bool suspended, power_state_present;
    char mode_description[128];
} epd_ssd2677_t;

esp_err_t epd_ssd2677_init(epd_ssd2677_t *display, const epd_ssd2677_config_t *config);
void epd_ssd2677_deinit(epd_ssd2677_t *display);
esp_err_t epd_ssd2677_resume(epd_ssd2677_t *display);
u8g2_t *epd_ssd2677_get_u8g2(epd_ssd2677_t *display);
esp_err_t epd_ssd2677_present_mono_xbm(epd_ssd2677_t *display,
    const uint8_t *bitmap, size_t bitmap_size, uint16_t x, uint16_t y,
    uint16_t width, uint16_t height, uint16_t stride, bool palette_inverted);
esp_err_t epd_ssd2677_present_power_state(epd_ssd2677_t *display);
esp_err_t epd_ssd2677_relax(epd_ssd2677_t *display, solar_os_display_relax_phase_t phase);
const char *epd_ssd2677_controller_mode(const epd_ssd2677_t *display);
const char *epd_ssd2677_controller_mode_values(const epd_ssd2677_t *display);
esp_err_t epd_ssd2677_set_controller_mode(epd_ssd2677_t *display, const char *mode);
