#include "solar_os_audio_pcm.h"

#include <string.h>

static int16_t audio_pcm_lerp_i16(int16_t a, int16_t b, uint32_t fraction_q16)
{
    const int32_t delta = (int32_t)b - (int32_t)a;
    return (int16_t)((int32_t)a +
                     (int32_t)(((int64_t)delta * fraction_q16) >> 16));
}

void solar_os_audio_s16_converter_reset(
    solar_os_audio_s16_converter_t *converter)
{
    if (converter != NULL) {
        memset(converter, 0, sizeof(*converter));
    }
}

esp_err_t solar_os_audio_s16_convert(
    solar_os_audio_s16_converter_t *converter,
    const int16_t *input,
    size_t input_frames,
    const solar_os_stream_audio_format_t *source,
    const solar_os_stream_audio_format_t *target,
    int16_t *output,
    size_t output_capacity_samples,
    size_t *output_samples,
    bool *source_done)
{
    if (converter == NULL || input == NULL || input_frames == 0U ||
        source == NULL || target == NULL || output == NULL ||
        output_samples == NULL || source_done == NULL ||
        source->sample_format != SOLAR_OS_STREAM_AUDIO_S16_LE ||
        target->sample_format != SOLAR_OS_STREAM_AUDIO_S16_LE ||
        source->sample_rate == 0U || target->sample_rate == 0U ||
        source->channels == 0U || source->channels > 2U ||
        target->channels == 0U || target->channels > 2U ||
        source->bits_per_sample != 16U || target->bits_per_sample != 16U ||
        output_capacity_samples < target->channels) {
        return ESP_ERR_INVALID_ARG;
    }

    if (converter->sample_rate != source->sample_rate ||
        converter->channels != source->channels) {
        converter->sample_rate = source->sample_rate;
        converter->channels = source->channels;
        converter->phase_q16 = 0U;
    }

    uint64_t phase = converter->phase_q16;
    const uint64_t limit = (uint64_t)input_frames << 16;
    uint64_t step = ((uint64_t)source->sample_rate << 16) /
        target->sample_rate;
    if (step == 0U) {
        step = 1U;
    }

    size_t produced = 0U;
    const size_t output_frame_capacity =
        output_capacity_samples / target->channels;
    /* Whole-frame, equal-rate blocks need no interpolation or per-sample
     * 64-bit phase arithmetic. Preserve partial-block continuation exactly;
     * fractional phase left by a previous resampling ratio uses the slow path. */
    if (source->sample_rate == target->sample_rate &&
        (phase & 0xffffU) == 0U && phase < limit) {
        const size_t start = (size_t)(phase >> 16);
        size_t frames = input_frames - start;
        if (frames > output_frame_capacity) frames = output_frame_capacity;
        const int16_t *samples = input + start * source->channels;
        if (source->channels == target->channels) {
            memmove(output, samples, frames * source->channels * sizeof(*output));
        } else if (target->channels == 1U) {
            for (size_t i = 0; i < frames; i++)
                output[i] = (int16_t)(((int32_t)samples[2U * i] + samples[2U * i + 1U]) / 2);
        } else {
            for (size_t i = 0; i < frames; i++) {
                output[2U * i] = samples[i];
                output[2U * i + 1U] = samples[i];
            }
        }
        phase += (uint64_t)frames << 16;
        *source_done = phase >= limit;
        converter->phase_q16 = *source_done ? phase - limit : phase;
        *output_samples = frames * target->channels;
        return ESP_OK;
    }
    while (phase < limit && produced / target->channels < output_frame_capacity) {
        const size_t index = (size_t)(phase >> 16);
        const size_t next_index = index + 1U < input_frames ? index + 1U : index;
        const uint32_t fraction = (uint32_t)(phase & 0xffffU);

        const int16_t left_a = input[index * source->channels];
        const int16_t left_b = input[next_index * source->channels];
        const int16_t left = audio_pcm_lerp_i16(left_a, left_b, fraction);
        int16_t right = left;
        if (source->channels > 1U) {
            const int16_t right_a = input[(index * source->channels) + 1U];
            const int16_t right_b = input[(next_index * source->channels) + 1U];
            right = audio_pcm_lerp_i16(right_a, right_b, fraction);
        }

        if (target->channels == 1U) {
            output[produced++] =
                (int16_t)(((int32_t)left + (int32_t)right) / 2);
        } else {
            output[produced++] = left;
            output[produced++] = right;
        }
        phase += step;
    }

    *source_done = phase >= limit;
    converter->phase_q16 = *source_done ? phase - limit : phase;
    *output_samples = produced;
    return produced > 0U ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
