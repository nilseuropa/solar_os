#pragma once
#include "solar_os_native_service_abi.h"
#include "solar_os_dsp_types_v1.h"

#define SOLAR_OS_NATIVE_DSP_SERVICE "dsp"
#define SOLAR_OS_NATIVE_DSP_ABI 1U

/* Buffers contain count signed 16-bit samples. Destination/input may be the
 * same buffer where supported by the operation; partial overlap is rejected.
 * Coefficients are copied at creation. FIR/decimator/FFT tokens own core state;
 * serialize operations on a token, join users and close it before unloading.
 * Close refuses active references. Size queries return zero for invalid tokens.
 * Returned backend names belong to the core and remain valid for this boot. */
typedef struct {
    uint32_t abi_version, struct_size;
    const char *(*backend)(void);
    uint32_t (*capabilities)(void);
    uint32_t (*accelerated_capabilities)(void);
    int (*dot_s16)(const int16_t *a, const int16_t *b, size_t count, int64_t *result);
    int (*gain_q15)(int16_t *dst, const int16_t *src, size_t count, int16_t gain);
    int (*mix_q15)(int16_t *dst, const int16_t *a, const int16_t *b, size_t count, int16_t gain_a, int16_t gain_b);
    int (*clip_s16)(int16_t *dst, const int16_t *src, size_t count, int16_t minimum, int16_t maximum);
    int (*level_s16)(const int16_t *src, size_t count, solar_os_dsp_level_t *level);
    int (*window_q15)(int16_t *dst, const int16_t *src, const int16_t *window, size_t count);
    int (*fir_create)(const int16_t *coefficients, size_t taps, solar_os_native_handle_t *out);
    int (*fir_close)(solar_os_native_handle_t fir);
    int (*fir_reset)(solar_os_native_handle_t fir);
    size_t (*fir_taps)(solar_os_native_handle_t fir);
    int (*fir_process)(solar_os_native_handle_t fir, int16_t *dst, const int16_t *src, size_t count);
    int (*decimator_create)(const int16_t *coefficients, size_t taps, size_t factor, solar_os_native_handle_t *out);
    int (*decimator_close)(solar_os_native_handle_t decimator);
    int (*decimator_reset)(solar_os_native_handle_t decimator);
    size_t (*decimator_factor)(solar_os_native_handle_t decimator);
    size_t (*decimator_output_count)(solar_os_native_handle_t decimator, size_t input_count);
    int (*decimator_process)(solar_os_native_handle_t decimator, int16_t *dst, size_t capacity,
        const int16_t *src, size_t count, size_t *produced);
    int (*fft_create)(size_t size, solar_os_native_handle_t *out);
    int (*fft_close)(solar_os_native_handle_t fft);
    size_t (*fft_size)(solar_os_native_handle_t fft);
    int (*fft_execute)(solar_os_native_handle_t fft, solar_os_dsp_complex_s16_t *output,
        const int16_t *input, uint8_t *scale_exponent);
} solar_os_native_dsp_api_v1_t;
