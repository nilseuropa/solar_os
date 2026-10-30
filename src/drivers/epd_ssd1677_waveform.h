#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SOLAR_OS_SSD1677_LUT_BYTES 105U

bool solar_os_ssd1677_waveform_build(uint8_t lut[SOLAR_OS_SSD1677_LUT_BYTES],
                                    unsigned drive_frames, unsigned fr, bool invert);
