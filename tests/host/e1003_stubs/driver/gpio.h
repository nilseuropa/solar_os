#pragma once
#include "../../epd_stubs/driver/gpio.h"
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode);
