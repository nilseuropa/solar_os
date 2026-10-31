#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "solar_os_audio_codec.h"
#include "solar_os_audio_pcm.h"

static void test_pcm_conversion(void)
{
    const solar_os_stream_audio_format_t source = {
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
        .sample_rate = 16000U,
        .channels = 2U,
        .bits_per_sample = 16U,
    };
    const solar_os_stream_audio_format_t target = {
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
        .sample_rate = 16000U,
        .channels = 1U,
        .bits_per_sample = 16U,
    };
    const int16_t input[] = {1000, -1000, 3000, 1000};
    int16_t output[2] = {0};
    size_t output_samples = 0U;
    bool source_done = false;
    solar_os_audio_s16_converter_t converter = {0};
    assert(solar_os_audio_s16_convert(&converter,
                                      input,
                                      2U,
                                      &source,
                                      &target,
                                      output,
                                      2U,
                                      &output_samples,
                                      &source_done) == ESP_OK);
    assert(source_done);
    assert(output_samples == 2U);
    assert(output[0] == 0);
    assert(output[1] == 2000);
}

static void test_pcm_mono_to_stereo_conversion(void)
{
    const solar_os_stream_audio_format_t source = {
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
        .sample_rate = 16000U,
        .channels = 1U,
        .bits_per_sample = 16U,
    };
    const solar_os_stream_audio_format_t target = {
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
        .sample_rate = 16000U,
        .channels = 2U,
        .bits_per_sample = 16U,
    };
    const int16_t input[] = {-2000, 3000};
    int16_t output[4] = {0};
    size_t output_samples = 0U;
    bool source_done = false;
    solar_os_audio_s16_converter_t converter = {0};
    assert(solar_os_audio_s16_convert(&converter,
                                      input,
                                      2U,
                                      &source,
                                      &target,
                                      output,
                                      4U,
                                      &output_samples,
                                      &source_done) == ESP_OK);
    assert(source_done);
    assert(output_samples == 4U);
    assert(output[0] == -2000);
    assert(output[1] == -2000);
    assert(output[2] == 3000);
    assert(output[3] == 3000);
}

static void test_pcm_equal_rate_continuation(void)
{
    int16_t input[62];
    for (size_t i = 0; i < 62; i++) input[i] = (int16_t)(i * 6553U + 32768U);
    for (unsigned in_channels = 1; in_channels <= 2; in_channels++) {
        for (unsigned out_channels = 1; out_channels <= 2; out_channels++) {
            solar_os_stream_audio_format_t source = {
                .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE, .sample_rate = 44100,
                .bits_per_sample = 16, .channels = in_channels,
            };
            solar_os_stream_audio_format_t target = source;
            target.channels = out_channels;
            for (size_t capacity = out_channels; capacity < 12; capacity++) {
                solar_os_audio_s16_converter_t converter = {0};
                size_t position = 0;
                bool done = false;
                while (!done) {
                    int16_t output[12];
                    for (size_t i = 0; i < 12; i++) output[i] = 12345;
                    size_t count = 0;
                    assert(solar_os_audio_s16_convert(&converter, input, 31, &source,
                        &target, output, capacity, &count, &done) == ESP_OK);
                    assert(count && count <= capacity && count % out_channels == 0);
                    for (size_t i = 0; i < count / out_channels; i++, position++) {
                        int16_t left = input[position * in_channels];
                        int16_t right = in_channels == 2 ? input[position * 2 + 1] : left;
                        assert(output[i * out_channels] == (out_channels == 1 ?
                            (int16_t)(((int32_t)left + right) / 2) : left));
                        if (out_channels == 2) assert(output[i * 2 + 1] == right);
                    }
                    assert(output[count] == 12345);
                    assert(converter.phase_q16 == (done ? 0U : (uint64_t)position << 16));
                }
                assert(position == 31);
            }
        }
    }
    /* Fractional carry must still interpolate at equal rates. */
    solar_os_stream_audio_format_t format = {
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE, .sample_rate = 16000,
        .bits_per_sample = 16, .channels = 1,
    };
    solar_os_audio_s16_converter_t converter = {.sample_rate = 16000, .channels = 1, .phase_q16 = 32768};
    const int16_t fractional[] = {0, 1000};
    int16_t output[2]; size_t count; bool done;
    assert(solar_os_audio_s16_convert(&converter, fractional, 2, &format, &format,
        output, 2, &count, &done) == ESP_OK);
    assert(done && count == 2 && output[0] == 500 && output[1] == 1000);
    assert(converter.phase_q16 == 32768);
}

int main(void)
{
    test_pcm_equal_rate_continuation();
    test_pcm_conversion();
    test_pcm_mono_to_stereo_conversion();
    const uint8_t mp3_header[] = {0xffU, 0xfbU, 0x90U, 0x64U};
    solar_os_stream_audio_format_t format;
    assert(solar_os_audio_mp3_probe(mp3_header, sizeof(mp3_header), &format) == ESP_OK);
    assert(format.sample_format == SOLAR_OS_STREAM_AUDIO_S16_LE);
    assert(format.sample_rate == 44100U);
    assert(format.channels == 2U);
    assert(format.bits_per_sample == 16U);

    const uint8_t invalid[] = {0x00U, 0x11U, 0x22U, 0x33U};
    assert(solar_os_audio_mp3_probe(invalid, sizeof(invalid), &format) ==
           ESP_ERR_INVALID_RESPONSE);

    solar_os_audio_mp3_decoder_t *decoder = NULL;
    assert(solar_os_audio_mp3_decoder_create(&decoder) == ESP_OK);
    assert(decoder != NULL);
    int16_t pcm[SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES];
    size_t consumed = 0U;
    solar_os_audio_decoded_frame_t frame;
    assert(solar_os_audio_mp3_decode(decoder,
                                     invalid,
                                     sizeof(invalid),
                                     &consumed,
                                     pcm,
                                     SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES,
                                     &frame) == ESP_OK);
    assert(frame.frames == 0U);
    solar_os_audio_mp3_decoder_destroy(decoder);
    return 0;
}
