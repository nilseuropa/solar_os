#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "u8g2.h"

typedef struct solar_os_board_display {
    void *driver;
    const char *driver_name;
    u8g2_t *u8g2;
    const char *controller;
    uint16_t width;
    uint16_t height;
    bool ready;
} solar_os_board_display_t;

esp_err_t solar_os_board_display_init(solar_os_board_display_t *display);
esp_err_t solar_os_board_display_resume(solar_os_board_display_t *display);
void solar_os_board_display_deinit(solar_os_board_display_t *display);
u8g2_t *solar_os_board_display_u8g2(solar_os_board_display_t *display);
const char *solar_os_board_display_driver_name(const solar_os_board_display_t *display);
const char *solar_os_board_display_controller(const solar_os_board_display_t *display);
uint16_t solar_os_board_display_width(const solar_os_board_display_t *display);
uint16_t solar_os_board_display_height(const solar_os_board_display_t *display);
bool solar_os_board_display_ready(const solar_os_board_display_t *display);
bool solar_os_board_display_brightness_supported(const solar_os_board_display_t *display);
esp_err_t solar_os_board_display_get_brightness(const solar_os_board_display_t *display,
                                                uint8_t *percent);
esp_err_t solar_os_board_display_set_brightness(solar_os_board_display_t *display,
                                                uint8_t percent);
const char *solar_os_board_display_controller_mode(const solar_os_board_display_t *display);
const char *solar_os_board_display_controller_mode_values(const solar_os_board_display_t *display);
esp_err_t solar_os_board_display_set_controller_mode(solar_os_board_display_t *display,
                                                     const char *mode);
