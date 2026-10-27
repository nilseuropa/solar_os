#pragma once
#include "esp_err.h"
typedef int gpio_num_t;
typedef enum { GPIO_MODE_INPUT } gpio_mode_t;
typedef enum { GPIO_FLOATING, GPIO_PULLUP_ONLY, GPIO_PULLDOWN_ONLY } gpio_pull_mode_t;
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0 && (pin) < 49)
esp_err_t gpio_set_pull_mode(gpio_num_t pin, gpio_pull_mode_t pull);
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode);
esp_err_t gpio_reset_pin(gpio_num_t pin);
