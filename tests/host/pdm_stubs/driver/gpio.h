#pragma once
#include "esp_err.h"
typedef int gpio_num_t;
#define GPIO_MODE_OUTPUT 1
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0 && (pin) < 49)
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) GPIO_IS_VALID_GPIO(pin)
esp_err_t gpio_set_level(int pin, int level);
esp_err_t gpio_set_direction(int pin, int mode);
