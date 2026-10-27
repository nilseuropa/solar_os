#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "u8g2.h"
#include "epd_relax.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EPD_SSD1677_REFRESH_AUTO = 0,
    EPD_SSD1677_REFRESH_PARTIAL,
    EPD_SSD1677_REFRESH_FULL,
} epd_ssd1677_refresh_mode_t;

typedef esp_err_t (*epd_ssd1677_power_fn_t)(void *context, bool on);

typedef struct {
    const char *spi_bus;
    spi_host_device_t spi_host;
    int sclk_pin;
    int mosi_pin;
    int cs_pin;
    int dc_pin;
    int reset_pin;
    int busy_pin;
    int power_pin;
    int spi_clock_hz;
    int busy_level;
    int power_active_level;
    const u8g2_cb_t *rotation;
    epd_ssd1677_power_fn_t set_power;
    void *power_context;
} epd_ssd1677_config_t;

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
    epd_ssd1677_refresh_mode_t refresh_mode;
    epd_relax_t relax;
    uint8_t partial_refresh_count;
    uint8_t refresh_log_count;
    int dc_pin;
    int reset_pin;
    int busy_pin;
    int power_pin;
    int busy_level;
    int power_active_level;
    epd_ssd1677_power_fn_t set_power;
    void *power_context;
    spi_host_device_t spi_host;
    bool bus_initialized;
    bool controller_ready;
    bool shadow_valid;
    bool partial_refresh_active;
    bool powered;
} epd_ssd1677_t;

esp_err_t epd_ssd1677_init(epd_ssd1677_t *display,
                           const epd_ssd1677_config_t *config);
esp_err_t epd_ssd1677_resume(epd_ssd1677_t *display);
void epd_ssd1677_deinit(epd_ssd1677_t *display);
esp_err_t epd_ssd1677_relax(epd_ssd1677_t *display, solar_os_display_relax_phase_t phase);
u8g2_t *epd_ssd1677_get_u8g2(epd_ssd1677_t *display);
const char *epd_ssd1677_controller_mode(const epd_ssd1677_t *display);
const char *epd_ssd1677_controller_mode_values(const epd_ssd1677_t *display);
esp_err_t epd_ssd1677_set_controller_mode(epd_ssd1677_t *display,
                                          const char *mode);

#ifdef __cplusplus
}
#endif
