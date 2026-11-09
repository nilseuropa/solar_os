#pragma once
#include "esp_err.h"
#include "driver/i2s_std.h"
#define GPIO_MODE_OUTPUT 2
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(int pin, unsigned level);
esp_err_t gpio_set_direction(int pin, int mode);
