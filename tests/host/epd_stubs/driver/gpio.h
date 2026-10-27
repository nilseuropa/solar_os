#pragma once
#include "../../gpio_controller_stubs/driver/gpio.h"
#define GPIO_NUM_MAX 49
#define GPIO_NUM_NC -1
esp_err_t gpio_reset_pin(gpio_num_t pin);
esp_err_t gpio_sleep_sel_dis(gpio_num_t pin);
esp_err_t gpio_hold_en(gpio_num_t pin);
esp_err_t gpio_hold_dis(gpio_num_t pin);
void gpio_deep_sleep_hold_en(void);
#define GPIO_PULLUP_ONLY 1
#define GPIO_FLOATING 0
esp_err_t gpio_set_pull_mode(gpio_num_t pin, int mode);
