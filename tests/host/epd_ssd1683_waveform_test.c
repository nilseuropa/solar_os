#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "epd_ssd1683_waveform.h"

/* Full 233-byte upstream default vector, Fastwave c7d20ee. Unspecified
 * entries, including static-pixel LUTs and unused groups, must stay zero. */
static const uint8_t default_waveform[233] = {
    [84] = 1, [86] = 0x4a, [87] = 1, [89] = 1, [90] = 1,
    [126] = 1, [128] = 0x8a, [129] = 1, [131] = 1, [132] = 1,
    [224] = 5, [227] = 0x22, [228] = 0x17,
    [229] = 0x41, [230] = 0xa8, [231] = 0x32, [232] = 0x08,
};

int main(void)
{
    uint8_t wave[233];
    solar_os_ssd1683_waveform_params_t p = solar_os_ssd1683_waveform_defaults();
    assert(solar_os_ssd1683_waveform_build(wave, &p));
    assert(memcmp(wave, default_waveform, sizeof(wave)) == 0);
    assert(solar_os_ssd1683_waveform_frames(wave) == 11);
    assert(p.volt_mask == SOLAR_OS_SSD1683_WAVEFORM_V_VCOM);

    p.reset_tp = 3; p.drive_tp = 8; p.settle_tp = 2;
    p.rp = 2; p.sr_ab = 3;
    assert(solar_os_ssd1683_waveform_build(wave, &p));
    assert(wave[85] == 0x83 && wave[86] == 0x48);
    assert(wave[127] == 0x43 && wave[128] == 0x88);
    assert(solar_os_ssd1683_waveform_frames(wave) == 70); /* 2*(3*(3+8)+2). */
    p.invert = true;
    assert(solar_os_ssd1683_waveform_build(wave, &p));
    assert(wave[85] == 0x43 && wave[86] == 0x88);
    assert(wave[127] == 0x83 && wave[128] == 0x48);

    p.reset_tp = 20; /* Opposite kick must stay shorter than target drive. */
    assert(solar_os_ssd1683_waveform_build(wave, &p));
    assert((wave[85] & 63) == 7 && (wave[127] & 63) == 7);
    p.rp = p.sr_ab = p.drive_tp = p.settle_tp = 0;
    assert(solar_os_ssd1683_waveform_build(wave, &p));
    assert(wave[84] == 1 && wave[85] == 0 && wave[86] == 0x81);
    assert(wave[87] == 0 && wave[89] == 1 && wave[90] == 0);
    assert(solar_os_ssd1683_waveform_frames(wave) == 1);

    p = solar_os_ssd1683_waveform_defaults();
    p.drive_tp = 255; p.settle_tp = 255;
    assert(solar_os_ssd1683_waveform_build(wave, &p));
    assert(wave[86] == 0x7f && wave[128] == 0xbf && wave[87] == 63);
    assert(solar_os_ssd1683_waveform_frames(wave) == 126);
    p.drive_tp = 63; p.reset_tp = 62; p.rp = p.sr_ab = 255;
    assert(solar_os_ssd1683_waveform_build(wave, &p));
    assert(solar_os_ssd1683_waveform_frames(wave) == UINT16_MAX);

    p = solar_os_ssd1683_waveform_defaults();
    p.drive_tp = 9; p.reset_tp = 2; p.balance_idle = true;
    assert(solar_os_ssd1683_waveform_build(wave, &p));
    assert(wave[42] == 1 && wave[43] == 0x45 && wave[44] == 0x85 && wave[45] == 2);
    assert(memcmp(wave + 42, wave + 168, 42) == 0);
    assert(solar_os_ssd1683_waveform_frames(wave) == 12);
    p.touch_bb = true;
    assert(solar_os_ssd1683_waveform_build(wave, &p));
    assert(wave[169] == 0x81 && wave[173] == 1);

    /* Blocks execute concurrently; groups within one block add their frames. */
    memset(wave, 0, sizeof(wave));
    wave[0] = 1; wave[1] = 0x7f; wave[5] = 1;
    wave[42] = 1; wave[43] = 0x81; wave[47] = 1;
    assert(solar_os_ssd1683_waveform_frames(wave) == 63);
    wave[7] = 2; wave[8] = 0xc2; wave[12] = 3;
    assert(solar_os_ssd1683_waveform_frames(wave) == 75);

    p = solar_os_ssd1683_waveform_defaults();
    for (unsigned fr = 0; fr < 256; ++fr) {
        p.fr = fr;
        const bool valid = (fr >= 1 && fr <= 5) || (fr >= 9 && fr <= 12);
        assert(solar_os_ssd1683_waveform_fr_valid(fr) == valid);
        assert(solar_os_ssd1683_waveform_build(wave, &p) == valid);
    }
    p.fr = 5; p.vcom = 0;
    assert(!solar_os_ssd1683_waveform_build(wave, &p));
    p.vcom = 0x79;
    assert(!solar_os_ssd1683_waveform_build(wave, &p));
    p.volt_mask = 0; /* Unselected trailer values must not be sent by adapter. */
    assert(solar_os_ssd1683_waveform_build(wave, &p));
    p.volt_mask = SOLAR_OS_SSD1683_WAVEFORM_V_SOURCE; p.vsh1 = 0x4c;
    assert(!solar_os_ssd1683_waveform_build(wave, &p));
    p.vsh1 = 0x41; p.vsl = 0x3b;
    assert(!solar_os_ssd1683_waveform_build(wave, &p));
    p.volt_mask = 0x80;
    assert(!solar_os_ssd1683_waveform_build(wave, &p));
    assert(!solar_os_ssd1683_waveform_build(NULL, &p));
    assert(!solar_os_ssd1683_waveform_build(wave, NULL));
    assert(solar_os_ssd1683_waveform_frames(NULL) == 0);
    puts("SSD1683 Fastwave builder vectors/timing/validation: ok");
    return 0;
}
