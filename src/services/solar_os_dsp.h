#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "solar_os_dsp_types_v1.h"

typedef struct solar_os_dsp_fir solar_os_dsp_fir_t;
typedef struct solar_os_dsp_decimator solar_os_dsp_decimator_t;
typedef struct solar_os_dsp_fft solar_os_dsp_fft_t;

/* Backend names and bitmasks are diagnostic. Callers select operations, not engines. */
const char *solar_os_dsp_backend(void);
uint32_t solar_os_dsp_capabilities(void);
uint32_t solar_os_dsp_accelerated_capabilities(void);

esp_err_t solar_os_dsp_dot_s16(const int16_t *a,
                               const int16_t *b,
                               size_t count,
                               int64_t *result);
esp_err_t solar_os_dsp_gain_q15(int16_t *dst,
                                const int16_t *src,
                                size_t count,
                                int16_t gain_q15);
esp_err_t solar_os_dsp_mix_q15(int16_t *dst,
                               const int16_t *a,
                               const int16_t *b,
                               size_t count,
                               int16_t gain_a_q15,
                               int16_t gain_b_q15);
esp_err_t solar_os_dsp_clip_s16(int16_t *dst,
                                const int16_t *src,
                                size_t count,
                                int16_t minimum,
                                int16_t maximum);
esp_err_t solar_os_dsp_level_s16(const int16_t *src,
                                 size_t count,
                                 solar_os_dsp_level_t *level);
esp_err_t solar_os_dsp_window_q15(int16_t *dst,
                                  const int16_t *src,
                                  const int16_t *window_q15,
                                  size_t count);

/* Coefficient zero multiplies the newest sample. Contexts are not reentrant. */
esp_err_t solar_os_dsp_fir_create(const int16_t *coefficients_q15,
                                  size_t taps,
                                  solar_os_dsp_fir_t **out_fir);
void solar_os_dsp_fir_destroy(solar_os_dsp_fir_t *fir);
void solar_os_dsp_fir_reset(solar_os_dsp_fir_t *fir);
size_t solar_os_dsp_fir_taps(const solar_os_dsp_fir_t *fir);
esp_err_t solar_os_dsp_fir_process(solar_os_dsp_fir_t *fir,
                                   int16_t *dst,
                                   const int16_t *src,
                                   size_t count);

esp_err_t solar_os_dsp_decimator_create(const int16_t *coefficients_q15,
                                        size_t taps,
                                        size_t factor,
                                        solar_os_dsp_decimator_t **out_decimator);
void solar_os_dsp_decimator_destroy(solar_os_dsp_decimator_t *decimator);
void solar_os_dsp_decimator_reset(solar_os_dsp_decimator_t *decimator);
size_t solar_os_dsp_decimator_factor(const solar_os_dsp_decimator_t *decimator);
size_t solar_os_dsp_decimator_output_count(const solar_os_dsp_decimator_t *decimator,
                                           size_t input_count);
esp_err_t solar_os_dsp_decimator_process(solar_os_dsp_decimator_t *decimator,
                                         int16_t *dst,
                                         size_t dst_capacity,
                                         const int16_t *src,
                                         size_t src_count,
                                         size_t *produced);

/* The normalized radix-2 FFT returns an interleaved complex result and log2(size). */
esp_err_t solar_os_dsp_fft_create(size_t size, solar_os_dsp_fft_t **out_fft);
void solar_os_dsp_fft_destroy(solar_os_dsp_fft_t *fft);
size_t solar_os_dsp_fft_size(const solar_os_dsp_fft_t *fft);
esp_err_t solar_os_dsp_fft_execute(solar_os_dsp_fft_t *fft,
                                   solar_os_dsp_complex_s16_t *output,
                                   const int16_t *input,
                                   uint8_t *scale_exponent);
