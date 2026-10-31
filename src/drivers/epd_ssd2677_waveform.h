#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SOLAR_OS_SSD2677_LUT_BYTES 535U

bool solar_os_ssd2677_waveform_build(uint8_t lut[SOLAR_OS_SSD2677_LUT_BYTES],
                                    unsigned frames, bool invert);
