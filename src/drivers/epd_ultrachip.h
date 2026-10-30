#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"
#include "u8g2.h"
#include "epd_relax.h"

/* Shared transport and monochrome framebuffer for 800x480 UltraChip panels.
 * Electrical settings and gate mapping belong to explicit panel profiles. */
#define EPD_ULTRACHIP_WIDTH 800U
#define EPD_ULTRACHIP_HEIGHT 480U
#define EPD_ULTRACHIP_ROW_BYTES 100U
#define EPD_ULTRACHIP_BUFFER_BYTES 48000U

typedef struct epd_ultrachip epd_ultrachip_t;
typedef struct {
    const char *name;
    uint16_t gate_count;
    uint16_t gate_offset;
    bool reverse_rows;
    esp_err_t (*initialize)(epd_ultrachip_t *display);
    esp_err_t (*activate)(epd_ultrachip_t *display, bool partial);
    esp_err_t (*finish)(epd_ultrachip_t *display);
} epd_ultrachip_panel_t;

typedef struct {
    const char *spi_bus;
    int cs_pin, dc_pin, reset_pin, busy_pin, power_pin;
    int spi_clock_hz;
    const u8g2_cb_t *rotation;
    unsigned panel;
} epd_ultrachip_config_t;

typedef enum {
    EPD_ULTRACHIP_REFRESH_AUTO,
    EPD_ULTRACHIP_REFRESH_PARTIAL,
    EPD_ULTRACHIP_REFRESH_FULL,
} epd_ultrachip_refresh_mode_t;

struct epd_ultrachip {
    spi_device_handle_t spi;
    u8g2_t u8g2;
    uint8_t *buffer, *shadow, *line_buffer;
    const epd_ultrachip_panel_t *panel;
    esp_err_t last_error;
    epd_ultrachip_refresh_mode_t refresh_mode;
    epd_relax_t relax;
    unsigned partial_count;
    int dc_pin, reset_pin, busy_pin, power_pin;
    bool controller_ready, shadow_valid, powered, analog_on, pins_configured;
};

esp_err_t epd_ultrachip_init(epd_ultrachip_t *display,
                            const epd_ultrachip_config_t *config,
                            const epd_ultrachip_panel_t *panel);
esp_err_t epd_ultrachip_resume(epd_ultrachip_t *display);
void epd_ultrachip_deinit(epd_ultrachip_t *display);
esp_err_t epd_ultrachip_relax(epd_ultrachip_t *display, solar_os_display_relax_phase_t phase);
u8g2_t *epd_ultrachip_get_u8g2(epd_ultrachip_t *display);
const char *epd_ultrachip_controller_mode(const epd_ultrachip_t *display);
const char *epd_ultrachip_controller_mode_values(const epd_ultrachip_t *display);
esp_err_t epd_ultrachip_set_controller_mode(epd_ultrachip_t *display, const char *mode);

/* Panel backend operations; errors propagate through the U8g2 callback. */
esp_err_t epd_ultrachip_command(epd_ultrachip_t *display, uint8_t command,
                               const uint8_t *data, size_t length);
esp_err_t epd_ultrachip_wait_ready(epd_ultrachip_t *display);
esp_err_t epd_ultrachip_power_on(epd_ultrachip_t *display);
esp_err_t epd_ultrachip_refresh(epd_ultrachip_t *display);
