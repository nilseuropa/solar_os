#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_client.h"
#include "solar_os_native_dsp.h"
#include "solar_os_native_handles.h"

static unsigned allocations;
static int fail_after = -1;
void *__real_calloc(size_t count, size_t size);
void __real_free(void *pointer);
void *__wrap_calloc(size_t count, size_t size)
{
    if (!fail_after) return NULL;
    if (fail_after > 0) --fail_after;
    void *pointer = __real_calloc(count, size);
    if (pointer) ++allocations;
    return pointer;
}
void __wrap_free(void *pointer)
{
    if (pointer) { assert(allocations); --allocations; }
    __real_free(pointer);
}

int main(void)
{
    const solar_os_native_dsp_api_v1_t *api = solar_os_native_dsp_get(solar_os_native_dsp_get_service, sizeof(*api));
#if SOLAR_OS_PACKAGE_SERVICE_DSP
    assert(api && api->backend() && api->capabilities() && !api->accelerated_capabilities());
    assert(!solar_os_native_dsp_get_service("dsp", 2, 8));
    assert(!solar_os_native_dsp_get_service("dsp", 1, sizeof(*api) + 1));
    const int16_t a[] = {INT16_MIN, -1000, 1000, INT16_MAX}, b[] = {INT16_MIN, 2000, -2000, INT16_MAX};
    int64_t dot;
    int16_t output[8] = {0};
    assert(!api->dot_s16(a, b, 4, &dot));
    assert(dot == (int64_t)INT16_MIN * INT16_MIN - 4000000LL + (int64_t)INT16_MAX * INT16_MAX);
    assert(!api->gain_q15(output, a, 4, 16384) && output[0] == -16384 && output[3] == 16383);
    assert(!api->mix_q15(output, a, b, 4, 32767, 32767) && output[0] == INT16_MIN && output[3] == INT16_MAX);
    assert(!api->clip_s16(output, a, 4, -900, 900) && output[0] == -900 && output[3] == 900);
    const int16_t levels[] = {-3, 4}, window[] = {32767, 16384, 0, INT16_MIN};
    solar_os_dsp_level_t level;
    assert(!api->level_s16(levels, 2, &level) && level.peak == 4 && level.rms == 3);
    assert(!api->window_q15(output, a, window, 4) && output[0] == -32767 && output[2] == 0);
    assert(api->gain_q15(output + 1, output, 4, 1) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->dot_s16(NULL, NULL, 0, &dot) && !dot);

    int16_t coefficients[] = {16384, 16384};
    const int16_t input[] = {1000, 2000, 3000, 4000}, expected[] = {500, 1500, 2500, 3500};
    solar_os_native_handle_t fir = 99;
    assert(api->fir_create(coefficients, 0, &fir) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !fir);
    for (int failure = 0; failure < 3; ++failure) {
        fail_after = failure;
        assert(api->fir_create(coefficients, 2, &fir) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !fir && !allocations);
    }
    fail_after = -1;
    assert(!api->fir_create(coefficients, 2, &fir) && fir && api->fir_taps(fir) == 2);
    coefficients[0] = 0;
    assert(!api->fir_process(fir, output, input, 4) && !memcmp(output, expected, sizeof(expected)));
    assert(!api->fir_reset(fir) && !api->fir_process(fir, output, input, 1) && output[0] == 500);
    assert(solar_os_native_handle_acquire(fir, SOLAR_OS_NATIVE_HANDLE_DSP_FIR));
    assert(api->fir_close(fir) == SOLAR_OS_NATIVE_ERROR_BUSY && api->fir_taps(fir) == 2);
    solar_os_native_handle_release(fir);
    assert(!api->fir_close(fir) && !api->fir_taps(fir));
    assert(api->fir_process(fir, output, input, 1) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->fir_close(fir) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !allocations);

    const int16_t coefficient[] = {32767}, first[] = {10, 20, 30}, second[] = {40, 50, 60};
    solar_os_native_handle_t decimator;
    size_t produced;
    assert(!api->decimator_create(coefficient, 1, 2, &decimator) && api->decimator_factor(decimator) == 2);
    assert(api->decimator_output_count(decimator, 3) == 2);
    assert(api->fir_close(decimator) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->decimator_process(decimator, output, 1, first, 3, &produced) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
    assert(!api->decimator_process(decimator, output, 8, first, 3, &produced) && produced == 2 && output[0] == 9 && output[1] == 29);
    assert(api->decimator_output_count(decimator, 3) == 1);
    assert(!api->decimator_process(decimator, output, 8, second, 3, &produced) && produced == 1 && output[0] == 49);
    assert(!api->decimator_reset(decimator) && api->decimator_output_count(decimator, 3) == 2);
    assert(!api->decimator_close(decimator) && !allocations);

    solar_os_native_handle_t fft;
    assert(api->fft_create(3, &fft) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !fft);
    assert(!api->fft_create(8, &fft) && api->fft_size(fft) == 8);
    assert(api->fir_reset(fft) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    const int16_t impulse[] = {32767,0,0,0,0,0,0,0};
    solar_os_dsp_complex_s16_t transformed[8];
    uint8_t scale;
    assert(!api->fft_execute(fft, transformed, impulse, &scale) && scale == 3);
    for (size_t i = 0; i < 8; ++i) assert(transformed[i].real == 4095 && !transformed[i].imag);
    assert(!api->fft_close(fft) && !api->fft_size(fft) && !allocations);

    solar_os_native_handle_t tokens[512];
    size_t count = 0;
    while (count < sizeof(tokens) / sizeof(tokens[0])) {
        int result = api->fir_create(coefficient, 1, &tokens[count]);
        if (result == SOLAR_OS_NATIVE_ERROR_NO_MEMORY) break;
        assert(!result);
        ++count;
    }
    assert(count && count < sizeof(tokens) / sizeof(tokens[0]));
    unsigned allocated = allocations;
    assert(api->fir_create(coefficient, 1, &fir) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !fir && allocations == allocated);
    for (size_t i = 0; i < count; ++i) assert(!api->fir_close(tokens[i]));
    assert(!allocations && !solar_os_native_handle_count());
    puts("native DSP: arithmetic, copied coefficients, streaming, FFT, typed lifetimes and pool exhaustion passed");
#else
    assert(!api && !allocations);
#endif
    return 0;
}
