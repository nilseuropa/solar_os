#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *spi_bus;
    int cs_pin;
    int dc_pin;
    int reset_pin;
    uint32_t spi_clock_hz;
    const u8g2_cb_t *rotation;
} rlcd_st7305_config_t;

typedef struct {
    spi_device_handle_t spi;
    u8g2_t u8g2;
    uint8_t *buffer;
    uint8_t *shadow;
    size_t buffer_size;
    size_t shadow_size;
    uint64_t shadow_valid_rows;
    SemaphoreHandle_t lock;
    esp_timer_handle_t idle_lpm_timer;
    uint32_t idle_lpm_delay_ms;
    uint8_t lpm_frame_rate;
    uint8_t hpm_frame_rate;
    uint8_t power_policy;
    uint8_t high_refresh_saved_hpm_frame_rate;
    uint8_t high_refresh_saved_power_policy;
    uint16_t high_refresh_hz_tenths;
    uint16_t direct_x;
    uint16_t direct_y;
    uint16_t direct_width;
    uint16_t direct_height;
    esp_err_t last_error;
    bool frame_content_changed;
    bool direct_frame_valid;
    bool direct_palette_inverted;
    bool inverted;
    bool high_refresh_override;
    const char *controller_mode;
    rlcd_st7305_config_t config;
} rlcd_st7305_t;

esp_err_t rlcd_st7305_init(rlcd_st7305_t *display,
                           const rlcd_st7305_config_t *config);
esp_err_t rlcd_st7305_resume(rlcd_st7305_t *display);
void rlcd_st7305_deinit(rlcd_st7305_t *display);
u8g2_t *rlcd_st7305_get_u8g2(rlcd_st7305_t *display);
const char *rlcd_st7305_controller_mode(const rlcd_st7305_t *display);
const char *rlcd_st7305_controller_mode_values(const rlcd_st7305_t *display);
esp_err_t rlcd_st7305_set_controller_mode(rlcd_st7305_t *display, const char *mode);
esp_err_t rlcd_st7305_set_high_refresh_override(rlcd_st7305_t *display,
                                                bool enabled,
                                                uint16_t hz_tenths);
esp_err_t rlcd_st7305_present_mono_xbm(rlcd_st7305_t *display,
                                       const uint8_t *bitmap,
                                       size_t bitmap_size,
                                       uint16_t x,
                                       uint16_t y,
                                       uint16_t width,
                                       uint16_t height,
                                       uint16_t stride,
                                       bool palette_inverted);

#ifdef __cplusplus
}
#endif
