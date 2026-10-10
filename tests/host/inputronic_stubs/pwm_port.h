#pragma once
#include "driver/gpio.h"
esp_err_t pwm_port_set(gpio_num_t pin, uint32_t frequency, uint8_t percent);
esp_err_t pwm_port_stop(gpio_num_t pin);
