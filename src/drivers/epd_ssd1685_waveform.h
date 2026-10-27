/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Bootdsc
 * SolarOS SSD1685 adaptation of Fastwave's short drive/settle phases.
 * See epd_ssd1685_waveform.c and doc/licenses/fastwave-notice.txt.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SOLAR_OS_SSD1685_LUT_BYTES 227U

/* SSD1685 mode 1: LUTC, LUTR (hold), LUTW, LUTB, eight groups each.
 * The adapter supplies synthetic RAM masks, not old/new image planes.
 * Only volatile waveform registers are used; voltage rails remain from OTP.
 */
bool solar_os_ssd1685_waveform_build(uint8_t lut[SOLAR_OS_SSD1685_LUT_BYTES],
                                    unsigned drive_frames, unsigned fr, bool invert);
