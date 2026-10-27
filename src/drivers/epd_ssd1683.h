#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "epd_pixel_budget.h"
#include "epd_scan_worker.h"
#include "solar_os_display_maintenance.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EPD_SSD1683_REFRESH_AUTO = 0,
    EPD_SSD1683_REFRESH_FAST,
    EPD_SSD1683_REFRESH_FULL,
    EPD_SSD1683_REFRESH_PULSE,
} epd_ssd1683_refresh_mode_t;

typedef enum {
    EPD_SSD1683_PANEL_UNKNOWN = 0,
    EPD_SSD1683_PANEL_LEGACY,
    EPD_SSD1683_PANEL_GREEN_STICKER,
    EPD_SSD1683_PANEL_WAVESHARE_V2,
    EPD_SSD1683_PANEL_ELECROW_579_DUAL,
} epd_ssd1683_panel_variant_t;

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
    epd_ssd1683_panel_variant_t panel_variant;
    bool startup_pulse;
} epd_ssd1683_config_t;

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
    int cs_pin;
    int dc_pin;
    int reset_pin;
    int busy_pin;
    int power_pin;
    int busy_level;
    int power_active_level;
    spi_host_device_t spi_host;
    bool bus_initialized;
    bool controller_ready;
    bool shadow_valid;
    bool partial_refresh_active;
    bool powered;
    epd_pulse_tuning_t pulse_tuning;
    epd_pixel_budget_t pixel_budget;
    uint8_t *pulse_planes;
    bool pulse_window_valid;
    uint8_t pulse_x_start, pulse_x_end;
    uint16_t pulse_y_start, pulse_y_end;
    size_t pulse_mask_pending;
    bool pulse_mask_dirty;
    bool pulse_power_hold, pulse_ramp_fast, pulse_analog_on;
    bool scan_async;
    bool power_state_present;
    uint8_t *relax_storage;
    uint32_t scan_submit_ready;
    uint16_t scan_period_ms;
    uint32_t spi_clock_hz, pulse_spi_clock_hz, spi_applied_hz;
    int64_t pulse_last_drive_start, pulse_drive_interval;
    int64_t pulse_last_frame_start;
    epd_scan_worker_t scan_worker;
    uint32_t pulse_updates;
    bool pulse_lut_loaded;
    bool pulse_needs_cleanup;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
    char mode_description[640];
} epd_ssd1683_t;

esp_err_t epd_ssd1683_init(epd_ssd1683_t *display,
                           const epd_ssd1683_config_t *config);
esp_err_t epd_ssd1683_resume(epd_ssd1683_t *display);
/* Compose a complete frame with a white (or inverted black) border. Source
 * rows are byte-padded, LSB-first MONO1, with set bits representing black. */
esp_err_t epd_ssd1683_present_mono_xbm(epd_ssd1683_t *display,
                                      const uint8_t *bitmap, size_t bitmap_size,
                                      uint16_t x, uint16_t y,
                                      uint16_t width, uint16_t height,
                                      uint16_t stride, bool palette_inverted);
esp_err_t epd_ssd1683_present_power_state(epd_ssd1683_t *display);
esp_err_t epd_ssd1683_relax(epd_ssd1683_t *display,
                            solar_os_display_relax_phase_t phase);
void epd_ssd1683_deinit(epd_ssd1683_t *display);
u8g2_t *epd_ssd1683_get_u8g2(epd_ssd1683_t *display);
const char *epd_ssd1683_controller_name(const epd_ssd1683_t *display);
const char *epd_ssd1683_controller_mode(const epd_ssd1683_t *display);
const char *epd_ssd1683_controller_mode_values(const epd_ssd1683_t *display);
esp_err_t epd_ssd1683_set_controller_mode(epd_ssd1683_t *display, const char *mode);

#ifdef __cplusplus
}
#endif
