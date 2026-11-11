#pragma once

#include "esp_err.h"

typedef int gpio_num_t;
#define GPIO_MODE_OUTPUT 1

esp_err_t gpio_hold_dis(gpio_num_t pin);
esp_err_t gpio_sleep_sel_dis(gpio_num_t pin);
esp_err_t gpio_set_level(gpio_num_t pin, unsigned level);
esp_err_t gpio_set_direction(gpio_num_t pin, int mode);
