#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * GT911 capacitive touch controller (Waveshare ESP32-S3-Touch-LCD-5),
 * polled on the internal I2C bus. The chip latches one of two I2C
 * addresses (0x5D or 0x14) from its INT line at reset release -- the
 * reset line sits on the CH422G expander and is high from expander
 * init onward, so this driver simply probes both addresses.
 *
 * Registers are 16-bit big-endian addresses. The status register
 * (0x814E) must be acknowledged (written 0) after every ready frame,
 * or the controller stops updating.
 */
esp_err_t touch_gt911_init(void);
bool touch_gt911_available(void);
esp_err_t touch_gt911_read(bool *pressed, uint16_t *x, uint16_t *y);
