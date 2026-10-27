/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Bootdsc
 *
 * Adapted from Fastwave-Eink-Driver, commit
 * c7d20eeefd0b0a5015b335a54bb9b63b37bca88f:
 * https://github.com/bootdsc/Fastwave-Eink-Driver
 * ports/ssd1683-crowpanel-579/firmware/waveform.cpp (wave_defaults,
 * wave_build, phase/group helpers, and wave_frames).
 *
 * SolarOS changes: pure C API, no Arduino or transport dependency, explicit
 * argument/register validation, and SolarOS names. Register loading remains
 * in the panel adapter. License and NOTICE: doc/licenses/fastwave*.txt.
 */
#include "epd_ssd1683_waveform.h"

#include <stddef.h>
#include <string.h>

#define BLOCK_STRIDE 42U
#define GROUP_STRIDE 7U
enum { VS_VSS = 0, VS_VSH1 = 1, VS_VSL = 2 };
enum { WB_LUTWW = 1, WB_LUTBW = 2, WB_LUTWB = 3, WB_LUTBB = 4 };

static uint8_t phase(uint8_t vs, unsigned tp)
{
    if (tp > 63) tp = 63;
    return (uint8_t)(((vs & 3U) << 6) | tp);
}

static uint8_t *group_at(uint8_t *waveform, unsigned block, unsigned group)
{
    return waveform + block * BLOCK_STRIDE + group * GROUP_STRIDE;
}

bool solar_os_ssd1683_waveform_fr_valid(unsigned fr)
{
    return (fr >= 1 && fr <= 5) || (fr >= 9 && fr <= 12);
}

solar_os_ssd1683_waveform_params_t solar_os_ssd1683_waveform_defaults(void)
{
    return (solar_os_ssd1683_waveform_params_t){
        .fr = 5, .reset_tp = 0, .drive_tp = 10, .settle_tp = 1,
        .rp = 1, .sr_ab = 1, .volt_mask = SOLAR_OS_SSD1683_WAVEFORM_V_VCOM,
        .vsh1 = 0x41, .vsh2 = 0xa8, .vsl = 0x32, .vcom = 0x08,
        .vgh = 0x17, .eopt = 0x22,
    };
}

bool solar_os_ssd1683_waveform_build(
    uint8_t waveform[SOLAR_OS_SSD1683_WAVEFORM_BYTES],
    const solar_os_ssd1683_waveform_params_t *p)
{
    if (waveform == NULL || p == NULL || !solar_os_ssd1683_waveform_fr_valid(p->fr) ||
        (p->volt_mask & ~15U) != 0 ||
        ((p->volt_mask & SOLAR_OS_SSD1683_WAVEFORM_V_VCOM) &&
         (p->vcom < 0x08 || p->vcom > 0x78)) ||
        ((p->volt_mask & SOLAR_OS_SSD1683_WAVEFORM_V_SOURCE) &&
         (p->vsh1 > 0x4b || p->vsl > 0x3a))) return false;

    memset(waveform, 0, SOLAR_OS_SSD1683_WAVEFORM_BYTES);
    const uint8_t to_white = p->invert ? VS_VSL : VS_VSH1;
    const uint8_t to_black = p->invert ? VS_VSH1 : VS_VSL;
    const uint8_t rp = p->rp ? p->rp : 1;
    const uint8_t sr = p->sr_ab ? p->sr_ab : 1;
    const uint8_t drive = p->drive_tp ? p->drive_tp : 1;
    uint8_t reset = p->reset_tp;
    if (reset >= drive) reset = drive > 1 ? (uint8_t)(drive - 1) : 0;

    const struct { uint8_t block, target, kick; } lut[2] = {
        {WB_LUTBW, to_white, to_black},
        {WB_LUTWB, to_black, to_white},
    };
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t *g = group_at(waveform, lut[i].block, 0);
        g[0] = rp;
        g[1] = reset ? phase(lut[i].kick, reset) : 0;
        g[2] = phase(lut[i].target, drive);
        g[3] = p->settle_tp ? phase(VS_VSS, p->settle_tp) : 0;
        g[5] = sr;
        g[6] = p->settle_tp ? 1 : 0;
    }
    if (p->balance_idle) {
        const uint8_t span = (uint8_t)(reset + drive);
        const uint8_t half = span / 2U;
        const uint8_t rest = (uint8_t)(p->settle_tp + (span - 2U * half));
        for (unsigned b = 0; b < 2; ++b) {
            uint8_t *g = group_at(waveform, b ? WB_LUTBB : WB_LUTWW, 0);
            g[0] = rp;
            g[1] = half ? phase(VS_VSH1, half) : 0;
            g[2] = half ? phase(VS_VSL, half) : 0;
            g[3] = rest ? phase(VS_VSS, rest) : 0;
            g[5] = sr;
            g[6] = rest ? 1 : 0;
        }
    }
    if (p->touch_bb) {
        uint8_t *g = group_at(waveform, WB_LUTBB, 0);
        g[0] = 1;
        g[1] = phase(to_black, 1);
        g[5] = 1;
    }
    waveform[224] = p->fr;
    waveform[227] = p->eopt;
    waveform[228] = p->vgh;
    waveform[229] = p->vsh1;
    waveform[230] = p->vsh2;
    waveform[231] = p->vsl;
    waveform[232] = p->vcom;
    return true;
}

uint16_t solar_os_ssd1683_waveform_frames(
    const uint8_t waveform[SOLAR_OS_SSD1683_WAVEFORM_BYTES])
{
    if (waveform == NULL) return 0;
    uint32_t worst = 0;
    for (unsigned b = 0; b < 5; ++b) {
        uint32_t total = 0;
        for (unsigned group = 0; group < 6; ++group) {
            const uint8_t *g = waveform + b * BLOCK_STRIDE + group * GROUP_STRIDE;
            const uint16_t ab = ((g[1] & 0x3fU) + (g[2] & 0x3fU)) * g[5];
            const uint16_t cd = ((g[3] & 0x3fU) + (g[4] & 0x3fU)) * g[6];
            total += (uint32_t)g[0] * (ab + cd);
        }
        if (total > worst) worst = total;
    }
    return worst > UINT16_MAX ? UINT16_MAX : (uint16_t)worst;
}
