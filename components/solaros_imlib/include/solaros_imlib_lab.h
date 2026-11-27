#pragma once
#include <stdint.h>

typedef struct {
    int8_t a, b;
} solaros_imlib_lab_ab_t;

extern const solaros_imlib_lab_ab_t solaros_imlib_lab_ab[32768];
extern const uint16_t solaros_imlib_lab_l_offsets[4096];
extern const uint8_t solaros_imlib_lab_l_rows[];

static inline uint8_t solaros_imlib_l(uint16_t rgb565)
{
    const unsigned index = rgb565 >> 1U;
    return solaros_imlib_lab_l_rows[solaros_imlib_lab_l_offsets[index >> 3U] + (index & 7U)];
}
