#pragma once
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_DSP_Q15_ONE 32767
#define SOLAR_OS_DSP_MAX_TAPS 1024U
#define SOLAR_OS_DSP_FFT_MIN_SIZE 2U
#define SOLAR_OS_DSP_FFT_MAX_SIZE 4096U

typedef enum {
    SOLAR_OS_DSP_CAP_DOT_S16 = 1U << 0,
    SOLAR_OS_DSP_CAP_GAIN_Q15 = 1U << 1,
    SOLAR_OS_DSP_CAP_MIX_Q15 = 1U << 2,
    SOLAR_OS_DSP_CAP_CLIP_S16 = 1U << 3,
    SOLAR_OS_DSP_CAP_LEVEL_S16 = 1U << 4,
    SOLAR_OS_DSP_CAP_WINDOW_Q15 = 1U << 5,
    SOLAR_OS_DSP_CAP_FIR_Q15 = 1U << 6,
    SOLAR_OS_DSP_CAP_DECIMATOR_Q15 = 1U << 7,
    SOLAR_OS_DSP_CAP_FFT_S16 = 1U << 8,
} solar_os_dsp_capability_t;

typedef struct {
    uint32_t peak;
    uint32_t rms;
} solar_os_dsp_level_t;

typedef struct {
    int16_t real;
    int16_t imag;
} solar_os_dsp_complex_s16_t;
