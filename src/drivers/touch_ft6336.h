#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * FT6336U capacitive touch controller (M5Stack Core2 / CoreS3), on
 * the internal I2C bus at 0x38. Polled -- there is no interrupt
 * wiring and no system-wide touch input concept yet; applications
 * that want touch poll it themselves (e.g. from their TICK events).
 *
 * Coordinates are the panel's native landscape frame: x 0..319 left
 * to right, y 0..239 top to bottom, matching the default display
 * orientation. On Core2 the touch film extends below the LCD (the
 * printed circles), so y values up to ~279 can be reported. Rotated
 * terminals (setterm orientation) are not compensated here.
 */
esp_err_t touch_ft6336_init(void);
bool touch_ft6336_available(void);

/*
 * One poll: *pressed says whether a finger is currently down; x/y are
 * only valid while pressed. Returns an error only on I2C failure.
 */
esp_err_t touch_ft6336_read(bool *pressed, uint16_t *x, uint16_t *y);
