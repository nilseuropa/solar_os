#pragma once

#include "esp_err.h"

/*
 * Minimal AXP192 bring-up for M5Stack Core2.
 *
 * Core2 does not expose LCD RESET or LCD backlight power on any ESP32
 * GPIO: both are gated behind the AXP192 PMIC on the internal I2C bus
 * (SDA=GPIO21, SCL=GPIO22, I2C address 0x34). This driver only turns on
 * the rails the display needs to come out of reset and light up. It does
 * not implement battery telemetry, charge control, or the vibration
 * motor/speaker rails -- those are separate follow-up work.
 *
 * Call this once, before solar_os_board_display_init() runs the ILI9341
 * (ILI9342C-compatible) panel init sequence, and make sure i2c_bus_init()
 * has already run.
 */
esp_err_t pmic_axp192_core2_bringup(void);
