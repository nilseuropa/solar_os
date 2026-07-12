#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * AW9523B I2C GPIO expander driver for M5Stack CoreS3.
 *
 * Standard register-addressed I2C expander (single fixed address,
 * unlike CH422G's per-function-address scheme). This minimal port only
 * brings the chip up far enough to release LCD reset (P1.5); touch
 * reset/interrupt (P0.0/P1.2) is for whichever peripheral driver needs
 * it next.
 */
esp_err_t io_expander_aw9523b_init(void);

/*
 * Speaker amp (AW88298) enable, P0.2 -- already configured as an output
 * by io_expander_aw9523b_init(), idle low (amp off). Call with true
 * before talking to the AW88298 over I2C, matching M5Stack's own
 * enable-before-configure ordering.
 */
esp_err_t io_expander_aw9523b_set_speaker_enable(bool enable);

/*
 * Grove Port A 5V boost enable, P1.7 -- already configured as an
 * output by io_expander_aw9523b_init(), idle low (boost off, port
 * unpowered). BUS_EN (P0.1) is already left high at init time so the
 * I2C lines are always connected; this only gates the 5V rail, which
 * M5Stack's own firmware forces off whenever BUS_EN is off, so callers
 * don't need to touch BUS_EN separately.
 */
esp_err_t io_expander_aw9523b_set_grove_a_boost_enable(bool enable);
