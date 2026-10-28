#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "u8g2.h"
#include "epd_relax.h"
#include "epd_pixel_budget.h"
#include "epd_scan_worker.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EPD_SSD1677_REFRESH_AUTO = 0,
    EPD_SSD1677_REFRESH_PARTIAL,
    EPD_SSD1677_REFRESH_FULL,
    EPD_SSD1677_REFRESH_PULSE,
} epd_ssd1677_refresh_mode_t;

typedef esp_err_t (*epd_ssd1677_power_fn_t)(void *context, bool on);

#define EPD_SSD1677_PANEL_GENERIC 0U
#define EPD_SSD1677_PANEL_STICKY 1U

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
    unsigned panel;
    const u8g2_cb_t *rotation;
    epd_ssd1677_power_fn_t set_power;
    void *power_context;
} epd_ssd1677_config_t;

typedef struct {
    spi_device_handle_t spi;
    unsigned panel;
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
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t lock;
    StaticSemaphore_t mode_lock_storage;
    SemaphoreHandle_t mode_lock;
    epd_pulse_tuning_t pulse_tuning;
    epd_pixel_budget_t pixel_budget;
    epd_scan_worker_t scan_worker;
    uint8_t *pulse_planes;
    size_t pulse_mask_pending;
    uint8_t pulse_x_start, pulse_x_end;
    uint16_t pulse_y_start, pulse_y_end;
    bool pulse_window_valid;
    uint32_t pulse_updates, scan_submit_ready, pulse_spi_clock_hz;
    uint16_t scan_period_ms;
    int64_t pulse_last_frame_start;
    bool scan_async, suspended, power_state_present, pulse_power_hold;
    bool fastwave_loaded, pulse_needs_cleanup, pulse_mask_dirty, analog_on, invert;
    char mode_description[640];
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
esp_err_t epd_ssd1677_present_power_state(epd_ssd1677_t *display);
void epd_ssd1677_deinit(epd_ssd1677_t *display);
esp_err_t epd_ssd1677_relax(epd_ssd1677_t *display, solar_os_display_relax_phase_t phase);
u8g2_t *epd_ssd1677_get_u8g2(epd_ssd1677_t *display);
esp_err_t epd_ssd1677_present_mono_xbm(epd_ssd1677_t *display,
    const uint8_t *bitmap, size_t bitmap_size, uint16_t x, uint16_t y,
    uint16_t width, uint16_t height, uint16_t stride, bool palette_inverted);
const char *epd_ssd1677_controller_mode(const epd_ssd1677_t *display);
const char *epd_ssd1677_controller_mode_values(const epd_ssd1677_t *display);
esp_err_t epd_ssd1677_set_controller_mode(epd_ssd1677_t *display,
                                          const char *mode);

#ifdef __cplusplus
}
#endif
