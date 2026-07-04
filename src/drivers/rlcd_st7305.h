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
    esp_err_t last_error;
    bool bus_initialized;
    bool frame_content_changed;
    const char *controller_mode;
} rlcd_st7305_t;

esp_err_t rlcd_st7305_init(rlcd_st7305_t *display);
esp_err_t rlcd_st7305_resume(rlcd_st7305_t *display);
void rlcd_st7305_deinit(rlcd_st7305_t *display);
u8g2_t *rlcd_st7305_get_u8g2(rlcd_st7305_t *display);
const char *rlcd_st7305_controller_mode(const rlcd_st7305_t *display);
const char *rlcd_st7305_controller_mode_values(const rlcd_st7305_t *display);
esp_err_t rlcd_st7305_set_controller_mode(rlcd_st7305_t *display, const char *mode);

#ifdef __cplusplus
}
#endif
