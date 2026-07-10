#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * CH422G I2C GPIO expander driver.
 *
 * Unlike a normal I2C GPIO expander, CH422G does not use a
 * register-address protocol: each of its four functions (system
 * config, open-collector outputs, push-pull outputs, input read) is
 * addressed as its own I2C slave address, with the data byte sent/
 * received as a plain single-byte transfer. Only push-pull outputs
 * IO0-IO7 are exposed here (open-collector outputs and the input
 * register aren't used by anything in this port yet).
 */

esp_err_t io_expander_ch422g_init(void);
esp_err_t io_expander_ch422g_set_pin(uint8_t pin, bool level);
