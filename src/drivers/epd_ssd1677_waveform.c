/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Bootdsc
 * Adapted from Fastwave-Eink-Driver c7d20eeefd0b0a5015b335a54bb9b63b37bca88f,
 * ports/ssd1677-seeed-426/firmware/wave1677.cpp.
 * SolarOS: bounded tuning, one group execution (RP=0), factory voltages.
 * Layout: SSD1677 rev 1.0, sections 6.6-6.7; doc/licenses/fastwave-notice.txt.
 */
#include "epd_ssd1677_waveform.h"

#include <string.h>

bool solar_os_ssd1677_waveform_build(uint8_t lut[SOLAR_OS_SSD1677_LUT_BYTES],
                                    unsigned drive_frames, unsigned fr, bool invert)
{
    if (!lut || drive_frames < 1 || drive_frames > 32 || fr < 1 || fr > 8)
        return false;
    memset(lut, 0, SOLAR_OS_SSD1677_LUT_BYTES);
    /* Mode 2: LUT1=(old,new)01 white, LUT2=10 black; 00/11 hold at VSS. */
    lut[10] = invert ? 0x40 : 0x80;
    lut[20] = invert ? 0x80 : 0x40;
    lut[50] = (uint8_t)drive_frames;
    lut[51] = 1;
    /* RP=0 means one execution; unused groups have zero phase lengths. */
    memset(lut + 100, (int)((fr << 4) | fr), 5);
    return true;
}
