#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Minimal AXP2101 PMIC driver for M5Stack CoreS3.
 *
 * Handles the LDO rails this port actually needs (digital core, system
 * 3.3V, LCD digital VDD, and the DLDO1-fed boost converter driving the
 * LCD backlight). Battery charging, power-off, and the touch/speaker
 * rails (ALDO4, ALDO3) are follow-up work along with those peripherals
 * themselves.
 */
esp_err_t pmic_axp2101_cores3_bringup(void);
esp_err_t pmic_axp2101_set_backlight(uint8_t percent);
