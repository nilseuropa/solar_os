#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Board-agnostic polled touch input. Each board's touch driver
 * fragment (boards/drivers/touch_*.cmake) compiles exactly one
 * controller driver, and that driver implements these three symbols;
 * applications gate on SOLAR_OS_BOARD_HAS_TOUCH and poll from their
 * TICK events. There is no system-wide touch input concept yet.
 *
 * Coordinates are in the panel's viewed orientation, matching the
 * gfx layer's default width/height. Rotated terminals (setterm
 * orientation) are not compensated.
 */
esp_err_t solar_os_touch_init(void);
bool solar_os_touch_available(void);

/*
 * One poll: *pressed says whether a finger is currently down; x/y are
 * only valid while pressed. Returns an error only on I2C failure.
 */
esp_err_t solar_os_touch_read(bool *pressed, uint16_t *x, uint16_t *y);
