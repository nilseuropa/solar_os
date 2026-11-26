#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_stream.h"

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint64_t phase_q16;
} solar_os_audio_s16_converter_t;

void solar_os_audio_s16_converter_reset(
    solar_os_audio_s16_converter_t *converter);

/*
 * Convert one complete signed 16-bit PCM source block. Call repeatedly with
 * the same source block until source_done is true when the output buffer is
 * smaller than the converted block. Downsampling may consume a block with
 * zero output samples.
 */
esp_err_t solar_os_audio_s16_convert(
    solar_os_audio_s16_converter_t *converter,
    const int16_t *input,
    size_t input_frames,
    const solar_os_stream_audio_format_t *source,
    const solar_os_stream_audio_format_t *target,
    int16_t *output,
    size_t output_capacity_samples,
    size_t *output_samples,
    bool *source_done);
