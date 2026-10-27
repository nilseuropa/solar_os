#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "u8g2.h"
#include "epd_pixel_budget.h"
#include "epd_scan_worker.h"
#include "epd_relax.h"

/* Panel profiles include glass wiring and OTP waveform assumptions. */
#define EPD_SSD1685_PANEL_GDEY029T71H 1U
#define EPD_SSD1685_WIDTH 168U
#define EPD_SSD1685_HEIGHT 384U
#define EPD_SSD1685_ROW_BYTES (EPD_SSD1685_WIDTH / 8U)
#define EPD_SSD1685_BUFFER_BYTES (EPD_SSD1685_ROW_BYTES * EPD_SSD1685_HEIGHT)

typedef struct {
    const char *spi_bus;
    int cs_pin, dc_pin, reset_pin, busy_pin, power_pin;
    int spi_clock_hz;
    const u8g2_cb_t *rotation;
    unsigned panel;
} epd_ssd1685_config_t;

typedef enum {
    EPD_SSD1685_REFRESH_AUTO,
    EPD_SSD1685_REFRESH_PARTIAL,
    EPD_SSD1685_REFRESH_FULL,
    EPD_SSD1685_REFRESH_PULSE,
} epd_ssd1685_refresh_mode_t;

typedef struct {
    spi_device_handle_t spi;
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t lock;
    u8g2_t u8g2;
    uint8_t *buffer, *shadow, *line_buffer;
    esp_err_t last_error;
    epd_ssd1685_refresh_mode_t refresh_mode;
    unsigned partial_count;
    epd_pulse_tuning_t pulse_tuning;
    epd_pixel_budget_t pixel_budget;
    epd_scan_worker_t scan_worker;
    epd_relax_t relax;
    uint8_t *pulse_planes;
    size_t pulse_mask_pending;
    uint32_t pulse_updates, scan_submit_ready;
    uint16_t scan_period_ms;
    uint32_t pulse_spi_clock_hz;
    int64_t pulse_last_frame_start;
    int dc_pin, reset_pin, busy_pin, power_pin;
    bool controller_ready, shadow_valid, powered, analog_on, pins_configured;
    bool fastwave_loaded, pulse_needs_cleanup, invert;
    bool pulse_mask_dirty;
    bool scan_async, suspended, pulse_power_hold, pulse_ramp_fast;
    bool power_state_present;
    char mode_description[640];
} epd_ssd1685_t;

esp_err_t epd_ssd1685_init(epd_ssd1685_t *display, const epd_ssd1685_config_t *config);
esp_err_t epd_ssd1685_refresh(epd_ssd1685_t *display);
/* Join/discard asynchronous scans and complete a full refresh before returning. */
esp_err_t epd_ssd1685_present_power_state(epd_ssd1685_t *display);
esp_err_t epd_ssd1685_resume(epd_ssd1685_t *display);
void epd_ssd1685_deinit(epd_ssd1685_t *display);
esp_err_t epd_ssd1685_relax(epd_ssd1685_t *display, solar_os_display_relax_phase_t phase);
/* Compose a full frame with a background border. MONO1 rows are byte-padded,
 * LSB-first, with set bits representing black; coordinates follow rotation.
 * Pulse mode submits an immutable snapshot to the existing scan worker.
 */
esp_err_t epd_ssd1685_present_mono_xbm(epd_ssd1685_t *display,
                                       const uint8_t *bitmap, size_t bitmap_size,
                                       uint16_t x, uint16_t y, uint16_t width,
                                       uint16_t height, uint16_t stride, bool inverted);
u8g2_t *epd_ssd1685_get_u8g2(epd_ssd1685_t *display);
const char *epd_ssd1685_controller_mode(const epd_ssd1685_t *display);
const char *epd_ssd1685_controller_mode_values(const epd_ssd1685_t *display);
esp_err_t epd_ssd1685_set_controller_mode(epd_ssd1685_t *display, const char *mode);
