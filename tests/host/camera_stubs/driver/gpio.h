#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
typedef enum { GPIO_MODE_OUTPUT = 2 } gpio_mode_t;
typedef enum { GPIO_DRIVE_CAP_0, GPIO_DRIVE_CAP_1, GPIO_DRIVE_CAP_2,
               GPIO_DRIVE_CAP_3, GPIO_DRIVE_CAP_DEFAULT = GPIO_DRIVE_CAP_2 } gpio_drive_cap_t;
esp_err_t gpio_set_drive_capability(gpio_num_t pin, gpio_drive_cap_t drive);
esp_err_t gpio_reset_pin(gpio_num_t pin);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode);
