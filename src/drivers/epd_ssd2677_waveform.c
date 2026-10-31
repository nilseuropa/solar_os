/* SSD2677 RAM LUT transport derived from FreeInk f1e1d2be2594cc4750d4f9114d13c80845691775,
 * libs/display/FreeInkDisplay/src/lut/Ssd2677Luts.h (MIT).
 * See doc/licenses/freeink-ssd2677-notice.txt. Pulse counting/hold masks follow
 * SolarOS's SSD1677 Fastwave adapter; no SSD1677 register layout is reused.
 */
#include "epd_ssd2677_waveform.h"
#include <string.h>

static uint8_t encode_count(unsigned value)
{
    /* RAM-LUT byte permutation, calibrated with BUSY on Sticky V2: the
     * counter bits 0..5 map to transport bits 4,7,6,1,0,3. */
    static const uint8_t bits[] = {4,7,6,1,0,3};
    uint8_t encoded = 0xff;
    for (unsigned i = 0; i < 6; i++) if (value & (1U << i)) encoded ^= 1U << bits[i];
    return encoded;
}

bool solar_os_ssd2677_waveform_build(uint8_t lut[SOLAR_OS_SSD2677_LUT_BYTES],
                                    unsigned frames, bool invert)
{
    if (!lut || frames < 1 || frames > 32) return false;
    memset(lut,0xff,SOLAR_OS_SSD2677_LUT_BYTES);
    static const uint8_t header[] = {0x00,0x78,0x78,0x78,0x78,0x2c,0x08};
    memcpy(lut,header,sizeof(header));
    /* Six 88-byte tables: VCOM, codes 00/01/10/11, and power mode. Each
     * contains eight 11-byte groups. Unused groups remain disabled. */
    for (unsigned group = 0; group < 8; group++) lut[7U + group*11U + 1U] = 0x7f;
    for (unsigned table = 0; table < 6; table++) {
        uint8_t *group = lut + 7U + table*88U;
        group[0] = encode_count(frames);
        /* 00 and 11 follow the common electrode. Only 01 (white) and 10
         * (black) receive drive. The last active phase was isolated and
         * optically verified on Sticky V2, including incremental settling. */
        if (table == 0 || table == 1 || table == 4) group[1] = 0x7f;
        if (table == 2 || table == 3) {
            const bool white = (table == 2) != invert;
            group[1] = group[2] = white ? 0xd5 : 0xba;
        }
        group[9] = group[10] = 0xef;
    }
    return true;
}
