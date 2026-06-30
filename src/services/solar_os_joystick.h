#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    gpio_num_t pin;
    const char *name;
    uint8_t low_key;
    uint8_t high_key;
    uint16_t low_press;
    uint16_t low_release;
    uint16_t high_press;
    uint16_t high_release;
} solar_os_joystick_axis_def_t;

typedef struct {
    bool initialized;
    gpio_num_t pin;
    const char *name;
    int raw;
    bool raw_valid;
    esp_err_t read_error;
    int direction;
    uint8_t low_key;
    uint8_t high_key;
    uint16_t low_press;
    uint16_t low_release;
    uint16_t high_press;
    uint16_t high_release;
} solar_os_joystick_axis_status_t;

esp_err_t solar_os_joystick_init(void);
size_t solar_os_joystick_axis_count(void);
bool solar_os_joystick_get_axis_status(size_t index, solar_os_joystick_axis_status_t *status);
esp_err_t solar_os_joystick_calibrate_center(void);
esp_err_t solar_os_joystick_calibrate_reset(void);
size_t solar_os_joystick_read_chars(char *buffer, size_t buffer_len);
