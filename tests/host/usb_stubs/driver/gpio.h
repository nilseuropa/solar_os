#pragma once

#include "esp_err.h"
#include <stdint.h>

typedef int gpio_num_t;
#define GPIO_MODE_OUTPUT 1
#define GPIO_MODE_DISABLE 0
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *config);

esp_err_t gpio_hold_dis(gpio_num_t pin);
esp_err_t gpio_sleep_sel_dis(gpio_num_t pin);
esp_err_t gpio_set_level(gpio_num_t pin, unsigned level);
esp_err_t gpio_set_direction(gpio_num_t pin, int mode);
