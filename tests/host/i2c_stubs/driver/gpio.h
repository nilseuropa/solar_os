#pragma once
#include "esp_err.h"
typedef int gpio_num_t;
#define GPIO_NUM_NC (-1)
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0 && (pin) < 49)
#define GPIO_MODE_OUTPUT 1
esp_err_t gpio_reset_pin(gpio_num_t pin);
esp_err_t gpio_set_level(gpio_num_t pin, int level);
esp_err_t gpio_set_direction(gpio_num_t pin, int mode);
