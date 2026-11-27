#include "solar_os_native_dsp.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_dsp.h"
#include "solar_os_native_dsp_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_SERVICE_DSP
static int dsp_dot_s16(const int16_t *a, const int16_t *b, size_t count, int64_t *result)
{ return solar_os_native_result(solar_os_dsp_dot_s16(a, b, count, result)); }

static int dsp_gain_q15(int16_t *dst, const int16_t *src, size_t count, int16_t gain)
{ return solar_os_native_result(solar_os_dsp_gain_q15(dst, src, count, gain)); }

static int dsp_mix_q15(int16_t *dst, const int16_t *a, const int16_t *b, size_t count, int16_t gain_a, int16_t gain_b)
{ return solar_os_native_result(solar_os_dsp_mix_q15(dst, a, b, count, gain_a, gain_b)); }

static int dsp_clip_s16(int16_t *dst, const int16_t *src, size_t count, int16_t minimum, int16_t maximum)
{ return solar_os_native_result(solar_os_dsp_clip_s16(dst, src, count, minimum, maximum)); }

static int dsp_level_s16(const int16_t *src, size_t count, solar_os_dsp_level_t *level)
{ return solar_os_native_result(solar_os_dsp_level_s16(src, count, level)); }

static int dsp_window_q15(int16_t *dst, const int16_t *src, const int16_t *window, size_t count)
{ return solar_os_native_result(solar_os_dsp_window_q15(dst, src, window, count)); }

static int dsp_fir_destroy(void *state)
{ solar_os_dsp_fir_destroy(state); return SOLAR_OS_NATIVE_OK; }

static int dsp_fir_create(const int16_t *coefficients, size_t taps, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_dsp_fir_t *state = NULL;
    int result = solar_os_native_result(solar_os_dsp_fir_create(coefficients, taps, &state));
    if (result) return result;
    result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_DSP_FIR, state, dsp_fir_destroy, out);
    if (result) solar_os_dsp_fir_destroy(state);
    return result;
}

static int dsp_fir_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_DSP_FIR); }

static int dsp_fir_reset(solar_os_native_handle_t id)
{
    solar_os_dsp_fir_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DSP_FIR);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_dsp_fir_reset(state);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static size_t dsp_fir_taps(solar_os_native_handle_t id)
{
    solar_os_dsp_fir_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DSP_FIR);
    if (!state) return 0;
    size_t result = solar_os_dsp_fir_taps(state);
    solar_os_native_handle_release(id);
    return result;
}

static int dsp_fir_process(solar_os_native_handle_t id, int16_t *dst, const int16_t *src, size_t count)
{
    solar_os_dsp_fir_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DSP_FIR);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_dsp_fir_process(state, dst, src, count));
    solar_os_native_handle_release(id);
    return result;
}

static int dsp_decimator_destroy(void *state)
{ solar_os_dsp_decimator_destroy(state); return SOLAR_OS_NATIVE_OK; }

static int dsp_decimator_create(const int16_t *coefficients, size_t taps, size_t factor, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_dsp_decimator_t *state = NULL;
    int result = solar_os_native_result(solar_os_dsp_decimator_create(coefficients, taps, factor, &state));
    if (result) return result;
    result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_DSP_DECIMATOR, state, dsp_decimator_destroy, out);
    if (result) solar_os_dsp_decimator_destroy(state);
    return result;
}

static int dsp_decimator_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_DSP_DECIMATOR); }

static int dsp_decimator_reset(solar_os_native_handle_t id)
{
    solar_os_dsp_decimator_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DSP_DECIMATOR);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_dsp_decimator_reset(state);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static size_t dsp_decimator_factor(solar_os_native_handle_t id)
{
    solar_os_dsp_decimator_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DSP_DECIMATOR);
    if (!state) return 0;
    size_t result = solar_os_dsp_decimator_factor(state);
    solar_os_native_handle_release(id);
    return result;
}

static size_t dsp_decimator_output_count(solar_os_native_handle_t id, size_t count)
{
    solar_os_dsp_decimator_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DSP_DECIMATOR);
    if (!state) return 0;
    size_t result = solar_os_dsp_decimator_output_count(state, count);
    solar_os_native_handle_release(id);
    return result;
}

static int dsp_decimator_process(solar_os_native_handle_t id, int16_t *dst, size_t capacity, const int16_t *src, size_t count, size_t *produced)
{
    solar_os_dsp_decimator_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DSP_DECIMATOR);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_dsp_decimator_process(state, dst, capacity, src, count, produced));
    solar_os_native_handle_release(id);
    return result;
}

static int dsp_fft_destroy(void *state)
{ solar_os_dsp_fft_destroy(state); return SOLAR_OS_NATIVE_OK; }

static int dsp_fft_create(size_t size, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_dsp_fft_t *state = NULL;
    int result = solar_os_native_result(solar_os_dsp_fft_create(size, &state));
    if (result) return result;
    result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_DSP_FFT, state, dsp_fft_destroy, out);
    if (result) solar_os_dsp_fft_destroy(state);
    return result;
}

static int dsp_fft_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_DSP_FFT); }

static size_t dsp_fft_size(solar_os_native_handle_t id)
{
    solar_os_dsp_fft_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DSP_FFT);
    if (!state) return 0;
    size_t result = solar_os_dsp_fft_size(state);
    solar_os_native_handle_release(id);
    return result;
}

static int dsp_fft_execute(solar_os_native_handle_t id, solar_os_dsp_complex_s16_t *output, const int16_t *input, uint8_t *scale)
{
    solar_os_dsp_fft_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DSP_FFT);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_dsp_fft_execute(state, output, input, scale));
    solar_os_native_handle_release(id);
    return result;
}

static const solar_os_native_dsp_api_v1_t dsp_api = {
    .abi_version = SOLAR_OS_NATIVE_DSP_ABI, .struct_size = sizeof(dsp_api),
    .backend = solar_os_dsp_backend,
    .capabilities = solar_os_dsp_capabilities,
    .accelerated_capabilities = solar_os_dsp_accelerated_capabilities,
    .dot_s16 = dsp_dot_s16,
    .gain_q15 = dsp_gain_q15,
    .mix_q15 = dsp_mix_q15,
    .clip_s16 = dsp_clip_s16,
    .level_s16 = dsp_level_s16,
    .window_q15 = dsp_window_q15,
    .fir_create = dsp_fir_create,
    .fir_close = dsp_fir_close,
    .fir_reset = dsp_fir_reset,
    .fir_taps = dsp_fir_taps,
    .fir_process = dsp_fir_process,
    .decimator_create = dsp_decimator_create,
    .decimator_close = dsp_decimator_close,
    .decimator_reset = dsp_decimator_reset,
    .decimator_factor = dsp_decimator_factor,
    .decimator_output_count = dsp_decimator_output_count,
    .decimator_process = dsp_decimator_process,
    .fft_create = dsp_fft_create,
    .fft_close = dsp_fft_close,
    .fft_size = dsp_fft_size,
    .fft_execute = dsp_fft_execute,
};
#endif

const void *solar_os_native_dsp_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_dsp_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
