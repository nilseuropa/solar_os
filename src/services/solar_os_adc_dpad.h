#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    gpio_num_t pin;
    const char *name;
    uint8_t mid_key;
    uint8_t high_key;
    uint16_t idle_max;
    uint16_t mid_min;
    uint16_t mid_max;
    uint16_t high_min;
} solar_os_adc_dpad_axis_def_t;

typedef enum {
    SOLAR_OS_ADC_DPAD_ZONE_IDLE = 0,
    SOLAR_OS_ADC_DPAD_ZONE_MID,
    SOLAR_OS_ADC_DPAD_ZONE_HIGH,
} solar_os_adc_dpad_zone_t;

typedef struct {
    bool initialized;
    gpio_num_t pin;
    const char *name;
    int raw;
    bool raw_valid;
    esp_err_t read_error;
    solar_os_adc_dpad_zone_t zone;
    uint8_t mid_key;
    uint8_t high_key;
    uint16_t idle_max;
    uint16_t mid_min;
    uint16_t mid_max;
    uint16_t high_min;
} solar_os_adc_dpad_axis_status_t;

esp_err_t solar_os_adc_dpad_init(void);
size_t solar_os_adc_dpad_axis_count(void);
bool solar_os_adc_dpad_get_axis_status(size_t index, solar_os_adc_dpad_axis_status_t *status);
esp_err_t solar_os_adc_dpad_calibrate_idle(void);
esp_err_t solar_os_adc_dpad_calibrate_reset(void);
size_t solar_os_adc_dpad_read_chars(char *buffer, size_t buffer_len);
