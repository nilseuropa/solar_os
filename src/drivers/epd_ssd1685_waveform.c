/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Bootdsc
 *
 * Adapted from Fastwave-Eink-Driver's drive/settle phase construction:
 * https://github.com/bootdsc/Fastwave-Eink-Driver
 * commit c7d20eeefd0b0a5015b335a54bb9b63b37bca88f,
 * ports/ssd1683-crowpanel-579/firmware/waveform.cpp.
 *
 * SolarOS changes: SSD1685 mode-1 four-block/eight-group layout, synthetic
 * hold/white/black masks, bounded tuning, and factory source/gate voltages.
 * Register layout: SSD1685 rev 0.11, sections 6.5-6.7 and command 0x32.
 * https://www.laskakit.cz/user/related_files/ssd1685.pdf
 * This panel adaptation is experimental and requires visual validation.
 */
#include "epd_ssd1685_waveform.h"

#include <string.h>

bool solar_os_ssd1685_waveform_build(uint8_t lut[SOLAR_OS_SSD1685_LUT_BYTES],
                                    unsigned drive_frames, unsigned fr, bool invert)
{
    if (!lut || drive_frames < 1 || drive_frames > 32 || fr < 1 || fr > 15)
        return false;
    memset(lut, 0, SOLAR_OS_SSD1685_LUT_BYTES);
    /* Match the timing of the driven blocks at VSS in both VCOM and hold.
     * Each group: RP, phase A/B/C/D (VS:2, TP:6), SR_AB, SR_CD.
     * Source VSH1 is black, VSL is white; invert allows a glass polarity test.
     */
    for (unsigned block = 0; block < 4; ++block) {
        uint8_t *g = lut + block * 56U;
        unsigned voltage = 0;
        if (block == 2) voltage = invert ? 1U : 2U;
        if (block == 3) voltage = invert ? 2U : 1U;
        g[0] = 1;
        g[2] = (uint8_t)((voltage << 6) | drive_frames);
        g[3] = 1; /* one VSS settle frame */
        g[5] = g[6] = 1;
    }
    lut[224] = (uint8_t)fr;
    return true;
}
