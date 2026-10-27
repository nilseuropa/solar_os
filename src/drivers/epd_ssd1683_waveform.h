/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Bootdsc
 * SolarOS C adaptation of Fastwave's SSD1683 waveform builder.
 * See epd_ssd1683_waveform.c and doc/licenses/fastwave-notice.txt.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_SSD1683_LUT_BYTES 227U
#define SOLAR_OS_SSD1683_WAVEFORM_BYTES 233U

enum {
    SOLAR_OS_SSD1683_WAVEFORM_V_EOPT = 1U,
    SOLAR_OS_SSD1683_WAVEFORM_V_GATE = 2U,
    SOLAR_OS_SSD1683_WAVEFORM_V_SOURCE = 4U,
    SOLAR_OS_SSD1683_WAVEFORM_V_VCOM = 8U,
};

typedef struct {
    uint8_t fr;
    uint8_t reset_tp, drive_tp, settle_tp;
    uint8_t rp, sr_ab;
    bool invert, touch_bb, balance_idle;
    uint8_t volt_mask;
    uint8_t vsh1, vsh2, vsl, vcom, vgh, eopt;
} solar_os_ssd1683_waveform_params_t;

solar_os_ssd1683_waveform_params_t solar_os_ssd1683_waveform_defaults(void);
bool solar_os_ssd1683_waveform_fr_valid(unsigned fr);
/* Generates 227 LUT bytes followed by EOPT, VGH, VSH1/VSH2/VSL, and VCOM.
 * The adapter must write only voltage registers selected by volt_mask.
 * Transition-class names and polarity follow upstream; the adapter owns the
 * mapping from image/pulse masks to the controller's OLD/NEW bitplanes.
 */
bool solar_os_ssd1683_waveform_build(
    uint8_t waveform[SOLAR_OS_SSD1683_WAVEFORM_BYTES],
    const solar_os_ssd1683_waveform_params_t *params);
uint16_t solar_os_ssd1683_waveform_frames(
    const uint8_t waveform[SOLAR_OS_SSD1683_WAVEFORM_BYTES]);

#ifdef __cplusplus
}
#endif
