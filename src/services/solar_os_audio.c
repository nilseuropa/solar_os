#include "solar_os_audio.h"

#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "audio_codec_board.h"
#include "esp_heap_caps.h"
#include "solar_os_log.h"
#include "esp_timer.h"
#include "minimp3.h"
#include "solar_os_storage.h"

#define AUDIO_FRAME_CHUNK 256U
#define AUDIO_LEVEL_BUFFER_SAMPLES 512U
#define AUDIO_LOOPBACK_BUFFER_BYTES 2048U
#define AUDIO_TONE_AMPLITUDE 12000
#define AUDIO_WAV_HEADER_BYTES 44U
#define AUDIO_WAV_BUFFER_BYTES 4096U
#define AUDIO_WAV_PCM_FORMAT 1U
#define AUDIO_MP3_INPUT_BUFFER_BYTES 16384U
#define AUDIO_MP3_PROBE_SCAN_BYTES 65536U
#define AUDIO_MP3_OUTPUT_MIN_SAMPLE_RATE 8000U
#define AUDIO_MP3_OUTPUT_FRAMES_MAX \
    (((MINIMP3_MAX_SAMPLES_PER_FRAME * AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE) / \
      AUDIO_MP3_OUTPUT_MIN_SAMPLE_RATE) + 8U)
#define AUDIO_MP3_OUTPUT_SAMPLES_MAX \
    (AUDIO_MP3_OUTPUT_FRAMES_MAX * AUDIO_CODEC_BOARD_DEFAULT_CHANNELS)

static const char *TAG = "solar_os_audio";

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint64_t phase_q16;
} audio_mp3_resampler_t;

static uint8_t clamp_percent_u32(uint32_t value)
{
    return value > 100U ? 100U : (uint8_t)value;
}

static uint32_t audio_abs_i16(int16_t value)
{
    return value < 0 ? (uint32_t)(-(int32_t)value) : (uint32_t)value;
}

static uint16_t audio_get_u16le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t audio_get_u32le(const uint8_t *data)
{
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24);
}

static void audio_put_u16le(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xffU);
    data[1] = (uint8_t)(value >> 8);
}

static void audio_put_u32le(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xffU);
    data[1] = (uint8_t)((value >> 8) & 0xffU);
    data[2] = (uint8_t)((value >> 16) & 0xffU);
    data[3] = (uint8_t)((value >> 24) & 0xffU);
}

static void audio_wav_fill_native_info(solar_os_audio_wav_info_t *info, uint32_t data_bytes)
{
    memset(info, 0, sizeof(*info));
    info->sample_rate = AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE;
    info->channels = AUDIO_CODEC_BOARD_DEFAULT_CHANNELS;
    info->bits_per_sample = AUDIO_CODEC_BOARD_DEFAULT_BITS;
    info->block_align = (uint16_t)((info->channels * info->bits_per_sample) / 8U);
    info->data_bytes = data_bytes;
    if (info->block_align != 0 && info->sample_rate != 0) {
        const uint32_t frames = data_bytes / info->block_align;
        info->duration_ms = (uint32_t)(((uint64_t)frames * 1000U) / info->sample_rate);
    }
}

static void audio_wav_build_header(uint8_t header[AUDIO_WAV_HEADER_BYTES],
                                   const solar_os_audio_wav_info_t *info)
{
    memset(header, 0, AUDIO_WAV_HEADER_BYTES);
    memcpy(&header[0], "RIFF", 4);
    audio_put_u32le(&header[4], info->data_bytes + 36U);
    memcpy(&header[8], "WAVE", 4);
    memcpy(&header[12], "fmt ", 4);
    audio_put_u32le(&header[16], 16U);
    audio_put_u16le(&header[20], AUDIO_WAV_PCM_FORMAT);
    audio_put_u16le(&header[22], info->channels);
    audio_put_u32le(&header[24], info->sample_rate);
    audio_put_u32le(&header[28],
                    info->sample_rate * info->channels * ((uint32_t)info->bits_per_sample / 8U));
    audio_put_u16le(&header[32], info->block_align);
    audio_put_u16le(&header[34], info->bits_per_sample);
    memcpy(&header[36], "data", 4);
    audio_put_u32le(&header[40], info->data_bytes);
}

static bool audio_wav_is_native(const solar_os_audio_wav_info_t *info)
{
    return info != NULL &&
        info->sample_rate == AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE &&
        info->channels == AUDIO_CODEC_BOARD_DEFAULT_CHANNELS &&
        info->bits_per_sample == AUDIO_CODEC_BOARD_DEFAULT_BITS &&
        info->block_align == ((AUDIO_CODEC_BOARD_DEFAULT_CHANNELS *
                               AUDIO_CODEC_BOARD_DEFAULT_BITS) / 8U);
}

static bool audio_wav_should_cancel(const solar_os_audio_wav_options_t *options)
{
    return options != NULL &&
        options->should_cancel != NULL &&
        options->should_cancel(options->user);
}

static uint32_t audio_wav_progress_interval_ms(const solar_os_audio_wav_options_t *options)
{
    if (options == NULL || options->progress_interval_ms == 0) {
        return SOLAR_OS_AUDIO_WAV_DEFAULT_PROGRESS_MS;
    }
    return options->progress_interval_ms;
}

static void audio_wav_report_progress(const solar_os_audio_wav_options_t *options,
                                      const solar_os_audio_wav_info_t *info,
                                      bool done,
                                      bool cancelled)
{
    if (options == NULL || options->progress == NULL || info == NULL) {
        return;
    }

    solar_os_audio_wav_progress_t progress = {
        .info = *info,
        .done = done,
        .cancelled = cancelled,
    };
    options->progress(&progress, options->user);
}

static esp_err_t audio_wav_write_exact(FILE *file, const void *data, size_t len)
{
    if (file == NULL || data == NULL) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    return fwrite(data, 1, len, file) == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t audio_wav_read_exact(FILE *file, void *data, size_t len)
{
    if (file == NULL || data == NULL) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    return fread(data, 1, len, file) == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t audio_wav_read_info_from_file(FILE *file,
                                               solar_os_audio_wav_info_t *info,
                                               long *data_offset)
{
    uint8_t header[12];
    bool have_fmt = false;
    bool have_data = false;
    long found_data_offset = 0;
    uint32_t found_data_bytes = 0;

    if (file == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));
    if (audio_wav_read_exact(file, header, sizeof(header)) != ESP_OK ||
        memcmp(&header[0], "RIFF", 4) != 0 ||
        memcmp(&header[8], "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    while (!feof(file)) {
        uint8_t chunk[8];
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) {
            break;
        }

        const uint32_t chunk_size = audio_get_u32le(&chunk[4]);
        const long chunk_data_offset = ftell(file);
        if (chunk_data_offset < 0) {
            return ESP_FAIL;
        }

        if (memcmp(&chunk[0], "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (chunk_size < sizeof(fmt) ||
                audio_wav_read_exact(file, fmt, sizeof(fmt)) != ESP_OK) {
                return ESP_ERR_INVALID_RESPONSE;
            }

            const uint16_t format = audio_get_u16le(&fmt[0]);
            info->channels = (uint8_t)audio_get_u16le(&fmt[2]);
            info->sample_rate = audio_get_u32le(&fmt[4]);
            info->block_align = audio_get_u16le(&fmt[12]);
            info->bits_per_sample = (uint8_t)audio_get_u16le(&fmt[14]);
            if (format != AUDIO_WAV_PCM_FORMAT ||
                info->channels == 0 ||
                info->sample_rate == 0 ||
                info->block_align == 0 ||
                info->bits_per_sample == 0) {
                return ESP_ERR_NOT_SUPPORTED;
            }

            const long remaining = (long)chunk_size - (long)sizeof(fmt);
            if (remaining > 0 && fseek(file, remaining, SEEK_CUR) != 0) {
                return ESP_FAIL;
            }
            have_fmt = true;
        } else if (memcmp(&chunk[0], "data", 4) == 0) {
            found_data_offset = chunk_data_offset;
            found_data_bytes = chunk_size;
            have_data = true;
            if (fseek(file, chunk_size + (chunk_size & 1U), SEEK_CUR) != 0) {
                return ESP_FAIL;
            }
        } else if (fseek(file, chunk_size + (chunk_size & 1U), SEEK_CUR) != 0) {
            return ESP_FAIL;
        }

        if (have_fmt && have_data) {
            info->data_bytes = found_data_bytes;
            if (info->block_align != 0 && info->sample_rate != 0) {
                const uint32_t frames = info->data_bytes / info->block_align;
                info->duration_ms = (uint32_t)(((uint64_t)frames * 1000U) / info->sample_rate);
            }
            if (data_offset != NULL) {
                *data_offset = found_data_offset;
            }
            return ESP_OK;
        }
    }

    return ESP_ERR_INVALID_RESPONSE;
}

static void *audio_heap_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ptr;
}

static uint32_t audio_mp3_synchsafe_u32(const uint8_t data[4])
{
    return ((uint32_t)(data[0] & 0x7fU) << 21) |
        ((uint32_t)(data[1] & 0x7fU) << 14) |
        ((uint32_t)(data[2] & 0x7fU) << 7) |
        (uint32_t)(data[3] & 0x7fU);
}

static esp_err_t audio_mp3_seek_payload(FILE *file)
{
    uint8_t header[10];

    if (file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    const size_t got = fread(header, 1, sizeof(header), file);
    if (got < sizeof(header)) {
        if (ferror(file)) {
            return ESP_FAIL;
        }
        return fseek(file, 0, SEEK_SET) == 0 ? ESP_OK : ESP_FAIL;
    }

    if (memcmp(header, "ID3", 3) != 0) {
        return fseek(file, 0, SEEK_SET) == 0 ? ESP_OK : ESP_FAIL;
    }

    uint32_t tag_bytes = audio_mp3_synchsafe_u32(&header[6]) + sizeof(header);
    if ((header[5] & 0x10U) != 0) {
        tag_bytes += 10U;
    }
    return fseek(file, (long)tag_bytes, SEEK_SET) == 0 ? ESP_OK : ESP_FAIL;
}

static bool audio_mp3_parse_frame_header(uint32_t header, solar_os_audio_wav_info_t *info)
{
    if ((header & 0xffe00000U) != 0xffe00000U) {
        return false;
    }

    const uint8_t version = (uint8_t)((header >> 19) & 0x03U);
    const uint8_t layer = (uint8_t)((header >> 17) & 0x03U);
    const uint8_t bitrate_index = (uint8_t)((header >> 12) & 0x0fU);
    const uint8_t sample_rate_index = (uint8_t)((header >> 10) & 0x03U);
    if (version == 1 || layer != 1 || bitrate_index == 0 || bitrate_index == 15 ||
        sample_rate_index == 3) {
        return false;
    }

    static const uint32_t base_rates[] = {44100U, 48000U, 32000U};
    uint32_t sample_rate = base_rates[sample_rate_index];
    if (version == 2) {
        sample_rate /= 2U;
    } else if (version == 0) {
        sample_rate /= 4U;
    }
    if (sample_rate < AUDIO_MP3_OUTPUT_MIN_SAMPLE_RATE) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    info->sample_rate = sample_rate;
    info->channels = ((header >> 6) & 0x03U) == 3U ? 1U : 2U;
    info->bits_per_sample = 16U;
    info->block_align = (uint16_t)(info->channels * sizeof(int16_t));
    return true;
}

static esp_err_t audio_mp3_fill_input(FILE *file,
                                      uint8_t *input,
                                      size_t *input_len,
                                      bool *eof)
{
    if (file == NULL || input == NULL || input_len == NULL || eof == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*eof || *input_len >= AUDIO_MP3_INPUT_BUFFER_BYTES) {
        return ESP_OK;
    }

    const size_t wanted = AUDIO_MP3_INPUT_BUFFER_BYTES - *input_len;
    const size_t got = fread(input + *input_len, 1, wanted, file);
    *input_len += got;
    if (got < wanted) {
        if (ferror(file)) {
            return ESP_FAIL;
        }
        *eof = true;
    }
    return ESP_OK;
}

static void audio_mp3_consume_input(uint8_t *input, size_t *input_len, size_t consumed)
{
    if (input == NULL || input_len == NULL || consumed == 0) {
        return;
    }
    if (consumed >= *input_len) {
        *input_len = 0;
        return;
    }
    memmove(input, input + consumed, *input_len - consumed);
    *input_len -= consumed;
}

static int16_t audio_mp3_lerp_i16(int16_t a, int16_t b, uint32_t frac_q16)
{
    const int32_t delta = (int32_t)b - (int32_t)a;
    return (int16_t)((int32_t)a + (int32_t)(((int64_t)delta * frac_q16) >> 16));
}

static size_t audio_mp3_convert_to_native(const int16_t *input,
                                          int frames,
                                          int channels,
                                          int sample_rate,
                                          audio_mp3_resampler_t *resampler,
                                          int16_t *output,
                                          size_t output_samples_cap)
{
    if (input == NULL || frames <= 0 || output == NULL || resampler == NULL ||
        channels <= 0 || channels > 2 || sample_rate <= 0) {
        return 0;
    }

    const uint32_t source_frames = (uint32_t)frames;

    if (resampler->sample_rate != (uint32_t)sample_rate ||
        resampler->channels != (uint8_t)channels) {
        resampler->sample_rate = (uint32_t)sample_rate;
        resampler->channels = (uint8_t)channels;
        resampler->phase_q16 = 0;
    }

    uint64_t phase = resampler->phase_q16;
    const uint64_t limit = (uint64_t)source_frames << 16;
    uint64_t step = ((uint64_t)sample_rate << 16) / AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE;
    if (step == 0) {
        step = 1;
    }

    size_t out_samples = 0;
    const size_t out_frame_cap = output_samples_cap / AUDIO_CODEC_BOARD_DEFAULT_CHANNELS;
    while (phase < limit && (out_samples / AUDIO_CODEC_BOARD_DEFAULT_CHANNELS) < out_frame_cap) {
        const uint32_t idx = (uint32_t)(phase >> 16);
        const uint32_t next_idx = idx + 1U < source_frames ? idx + 1U : idx;
        const uint32_t frac = (uint32_t)(phase & 0xffffU);

        const int16_t left_a = input[idx * (uint32_t)channels];
        const int16_t left_b = input[next_idx * (uint32_t)channels];
        int16_t left = audio_mp3_lerp_i16(left_a, left_b, frac);
        int16_t right = left;
        if (channels > 1) {
            const int16_t right_a = input[(idx * (uint32_t)channels) + 1U];
            const int16_t right_b = input[(next_idx * (uint32_t)channels) + 1U];
            right = audio_mp3_lerp_i16(right_a, right_b, frac);
        }

        output[out_samples++] = left;
        output[out_samples++] = right;
        phase += step;
    }

    resampler->phase_q16 = phase >= limit ? phase - limit : phase;
    return out_samples;
}

static void audio_mp3_fill_native_info(solar_os_audio_wav_info_t *info, uint32_t data_bytes)
{
    audio_wav_fill_native_info(info, data_bytes);
}

static esp_err_t audio_mp3_probe_from_file(FILE *file, solar_os_audio_wav_info_t *info)
{
    esp_err_t ret = audio_mp3_seek_payload(file);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t buffer[512];
    size_t scanned = 0;
    size_t have = 0;
    uint32_t window = 0;

    while (scanned < AUDIO_MP3_PROBE_SCAN_BYTES) {
        const size_t remaining = AUDIO_MP3_PROBE_SCAN_BYTES - scanned;
        const size_t wanted = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
        const size_t got = fread(buffer, 1, wanted, file);
        if (got == 0) {
            if (ferror(file)) {
                return ESP_FAIL;
            }
            break;
        }

        for (size_t i = 0; i < got; i++) {
            window = (window << 8) | buffer[i];
            if (have < 4) {
                have++;
            }
            if (have >= 4 && audio_mp3_parse_frame_header(window, info)) {
                return ESP_OK;
            }
        }
        scanned += got;
    }

    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t solar_os_audio_init(void)
{
    return audio_codec_board_init();
}

void solar_os_audio_deinit(void)
{
    audio_codec_board_deinit();
}

esp_err_t solar_os_audio_set_volume(uint8_t volume)
{
    return audio_codec_board_set_volume(volume);
}

esp_err_t solar_os_audio_set_mic_gain(float gain_db)
{
    return audio_codec_board_set_mic_gain(gain_db);
}

esp_err_t solar_os_audio_play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume)
{
    if (frequency_hz < SOLAR_OS_AUDIO_TONE_MIN_HZ ||
        frequency_hz > SOLAR_OS_AUDIO_TONE_MAX_HZ ||
        duration_ms == 0 ||
        duration_ms > SOLAR_OS_AUDIO_TEST_MAX_MS ||
        volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = solar_os_audio_set_volume(volume);
    if (ret != ESP_OK) {
        return ret;
    }

    audio_codec_board_status_t status;
    audio_codec_board_get_status(&status);

    int16_t samples[AUDIO_FRAME_CHUNK * AUDIO_CODEC_BOARD_DEFAULT_CHANNELS];
    uint32_t phase = 0;
    const uint32_t phase_step = (uint32_t)(((uint64_t)frequency_hz << 16) / status.sample_rate);
    uint32_t frames_remaining = (uint32_t)(((uint64_t)status.sample_rate * duration_ms) / 1000U);

    while (frames_remaining > 0) {
        const uint32_t frames = frames_remaining > AUDIO_FRAME_CHUNK ?
            AUDIO_FRAME_CHUNK :
            frames_remaining;
        for (uint32_t frame = 0; frame < frames; frame++) {
            const int16_t sample = (phase & 0x8000U) ? AUDIO_TONE_AMPLITUDE : -AUDIO_TONE_AMPLITUDE;
            phase += phase_step;
            for (uint8_t ch = 0; ch < AUDIO_CODEC_BOARD_DEFAULT_CHANNELS; ch++) {
                samples[(frame * AUDIO_CODEC_BOARD_DEFAULT_CHANNELS) + ch] = sample;
            }
        }

        ret = audio_codec_board_write(samples,
                                      frames * AUDIO_CODEC_BOARD_DEFAULT_CHANNELS * sizeof(samples[0]));
        if (ret != ESP_OK) {
            return ret;
        }
        frames_remaining -= frames;
    }

    memset(samples, 0, sizeof(samples));
    (void)audio_codec_board_write(samples, sizeof(samples));
    SOLAR_OS_LOGI(TAG, "tone: %" PRIu32 " Hz %" PRIu32 " ms vol=%u",
             frequency_hz,
             duration_ms,
             volume);
    return ESP_OK;
}

esp_err_t solar_os_audio_measure_level(uint32_t duration_ms, solar_os_audio_level_t *level)
{
    if (duration_ms == 0 || duration_ms > SOLAR_OS_AUDIO_TEST_MAX_MS || level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(level, 0, sizeof(*level));
    esp_err_t ret = solar_os_audio_init();
    if (ret != ESP_OK) {
        return ret;
    }

    int16_t samples[AUDIO_LEVEL_BUFFER_SAMPLES];
    const int64_t deadline_us = esp_timer_get_time() + ((int64_t)duration_ms * 1000);
    uint64_t sum_abs = 0;
    uint32_t peak = 0;

    while (esp_timer_get_time() < deadline_us) {
        ret = audio_codec_board_read(samples, sizeof(samples));
        if (ret != ESP_OK) {
            return ret;
        }

        for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
            const uint32_t value = audio_abs_i16(samples[i]);
            if (value > peak) {
                peak = value;
            }
            sum_abs += value;
        }
        level->samples += sizeof(samples) / sizeof(samples[0]);
    }

    if (level->samples == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint32_t average = (uint32_t)(sum_abs / level->samples);
    level->peak_percent = clamp_percent_u32((peak * 100U) / 32767U);
    level->average_percent = clamp_percent_u32((average * 100U) / 32767U);
    SOLAR_OS_LOGI(TAG,
             "level: samples=%" PRIu32 " peak=%u avg=%u",
             level->samples,
             level->peak_percent,
             level->average_percent);
    return ESP_OK;
}

esp_err_t solar_os_audio_loopback(uint32_t duration_ms, uint8_t volume)
{
    if (duration_ms == 0 || duration_ms > SOLAR_OS_AUDIO_TEST_MAX_MS || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = solar_os_audio_set_volume(volume);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t *buffer = heap_caps_malloc(AUDIO_LOOPBACK_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(AUDIO_LOOPBACK_BUFFER_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const int64_t deadline_us = esp_timer_get_time() + ((int64_t)duration_ms * 1000);
    while (esp_timer_get_time() < deadline_us) {
        ret = audio_codec_board_read(buffer, AUDIO_LOOPBACK_BUFFER_BYTES);
        if (ret != ESP_OK) {
            break;
        }
        ret = audio_codec_board_write(buffer, AUDIO_LOOPBACK_BUFFER_BYTES);
        if (ret != ESP_OK) {
            break;
        }
    }

    heap_caps_free(buffer);
    SOLAR_OS_LOGI(TAG, "loopback: %" PRIu32 " ms vol=%u ret=%s",
             duration_ms,
             volume,
             esp_err_to_name(ret));
    return ret;
}

esp_err_t solar_os_audio_get_wav_info(const char *path, solar_os_audio_wav_info_t *info)
{
    if (path == NULL || path[0] == '\0' || info == NULL) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    const esp_err_t ret = audio_wav_read_info_from_file(file, info, NULL);
    const int close_errno = errno;
    fclose(file);
    errno = close_errno;
    return ret;
}

esp_err_t solar_os_audio_get_mp3_info(const char *path, solar_os_audio_wav_info_t *info)
{
    if (path == NULL || path[0] == '\0' || info == NULL) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    const esp_err_t ret = audio_mp3_probe_from_file(file, info);
    const int close_errno = errno;
    fclose(file);
    errno = close_errno;
    return ret;
}

esp_err_t solar_os_audio_record_wav(const char *path,
                                    uint32_t duration_ms,
                                    const solar_os_audio_wav_options_t *options,
                                    solar_os_audio_wav_info_t *info)
{
    if (path == NULL || path[0] == '\0' || duration_ms == 0 ||
        duration_ms > SOLAR_OS_AUDIO_WAV_MAX_MS) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = solar_os_audio_init();
    if (ret != ESP_OK) {
        return ret;
    }

    solar_os_audio_wav_info_t current;
    audio_wav_fill_native_info(&current, 0);
    const uint32_t frame_bytes = current.block_align;
    const uint32_t target_frames =
        (uint32_t)(((uint64_t)current.sample_rate * duration_ms) / 1000U);
    const uint64_t target_bytes64 = (uint64_t)target_frames * frame_bytes;
    if (target_bytes64 > UINT32_MAX - AUDIO_WAV_HEADER_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t target_bytes = (uint32_t)target_bytes64;

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    uint8_t header[AUDIO_WAV_HEADER_BYTES];
    audio_wav_build_header(header, &current);
    ret = audio_wav_write_exact(file, header, sizeof(header));
    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }

    uint8_t *buffer = heap_caps_malloc(AUDIO_WAV_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(AUDIO_WAV_BUFFER_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buffer == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    const uint32_t progress_interval_ms = audio_wav_progress_interval_ms(options);
    int64_t next_progress_us = esp_timer_get_time() + ((int64_t)progress_interval_ms * 1000);
    bool cancelled = false;

    while (current.data_bytes < target_bytes) {
        if (audio_wav_should_cancel(options)) {
            cancelled = true;
            ret = ESP_ERR_TIMEOUT;
            break;
        }

        uint32_t chunk = target_bytes - current.data_bytes;
        if (chunk > AUDIO_WAV_BUFFER_BYTES) {
            chunk = AUDIO_WAV_BUFFER_BYTES;
        }
        chunk -= chunk % frame_bytes;
        if (chunk == 0) {
            break;
        }

        ret = audio_codec_board_read(buffer, chunk);
        if (ret != ESP_OK) {
            break;
        }
        ret = audio_wav_write_exact(file, buffer, chunk);
        if (ret != ESP_OK) {
            break;
        }

        current.data_bytes += chunk;
        current.duration_ms =
            (uint32_t)((((uint64_t)current.data_bytes / frame_bytes) * 1000U) /
                       current.sample_rate);

        const int64_t now_us = esp_timer_get_time();
        if (now_us >= next_progress_us) {
            audio_wav_report_progress(options, &current, false, false);
            next_progress_us = now_us + ((int64_t)progress_interval_ms * 1000);
        }
    }

    audio_wav_build_header(header, &current);
    if (fseek(file, 0, SEEK_SET) != 0 ||
        audio_wav_write_exact(file, header, sizeof(header)) != ESP_OK ||
        fflush(file) != 0) {
        if (ret == ESP_OK || ret == ESP_ERR_TIMEOUT) {
            ret = ESP_FAIL;
        }
    }

    const int close_errno = errno;
    if (fclose(file) != 0 && (ret == ESP_OK || ret == ESP_ERR_TIMEOUT)) {
        ret = ESP_FAIL;
    }
    errno = close_errno;
    heap_caps_free(buffer);

    if (info != NULL) {
        *info = current;
    }
    audio_wav_report_progress(options, &current, true, cancelled);
    SOLAR_OS_LOGI(TAG,
             "record wav %s: bytes=%" PRIu32 " ms=%" PRIu32 " ret=%s",
             path,
             current.data_bytes,
             current.duration_ms,
             esp_err_to_name(ret));
    return ret;
}

esp_err_t solar_os_audio_play_wav(const char *path,
                                  uint8_t volume,
                                  const solar_os_audio_wav_options_t *options,
                                  solar_os_audio_wav_info_t *info)
{
    if (path == NULL || path[0] == '\0' || volume > 100) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    solar_os_audio_wav_info_t source;
    long data_offset = 0;
    esp_err_t ret = audio_wav_read_info_from_file(file, &source, &data_offset);
    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }
    if (!audio_wav_is_native(&source) || source.data_bytes == 0) {
        fclose(file);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (fseek(file, data_offset, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    ret = solar_os_audio_set_volume(volume);
    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }

    uint8_t *buffer = heap_caps_malloc(AUDIO_WAV_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(AUDIO_WAV_BUFFER_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buffer == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    solar_os_audio_wav_info_t progress = source;
    progress.data_bytes = 0;
    progress.duration_ms = 0;
    const uint32_t progress_interval_ms = audio_wav_progress_interval_ms(options);
    int64_t next_progress_us = esp_timer_get_time() + ((int64_t)progress_interval_ms * 1000);
    bool cancelled = false;

    while (progress.data_bytes < source.data_bytes) {
        if (audio_wav_should_cancel(options)) {
            cancelled = true;
            ret = ESP_ERR_TIMEOUT;
            break;
        }

        uint32_t chunk = source.data_bytes - progress.data_bytes;
        if (chunk > AUDIO_WAV_BUFFER_BYTES) {
            chunk = AUDIO_WAV_BUFFER_BYTES;
        }
        chunk -= chunk % source.block_align;
        if (chunk == 0) {
            break;
        }

        ret = audio_wav_read_exact(file, buffer, chunk);
        if (ret != ESP_OK) {
            break;
        }
        ret = audio_codec_board_write(buffer, chunk);
        if (ret != ESP_OK) {
            break;
        }

        progress.data_bytes += chunk;
        progress.duration_ms =
            (uint32_t)((((uint64_t)progress.data_bytes / progress.block_align) * 1000U) /
                       progress.sample_rate);

        const int64_t now_us = esp_timer_get_time();
        if (now_us >= next_progress_us) {
            audio_wav_report_progress(options, &progress, false, false);
            next_progress_us = now_us + ((int64_t)progress_interval_ms * 1000);
        }
    }

    memset(buffer, 0, AUDIO_WAV_BUFFER_BYTES);
    (void)audio_codec_board_write(buffer, AUDIO_WAV_BUFFER_BYTES);

    const int close_errno = errno;
    fclose(file);
    errno = close_errno;
    heap_caps_free(buffer);

    if (info != NULL) {
        *info = progress;
    }
    audio_wav_report_progress(options, &progress, true, cancelled);
    SOLAR_OS_LOGI(TAG,
             "play wav %s: bytes=%" PRIu32 "/%" PRIu32 " ms=%" PRIu32 " ret=%s",
             path,
             progress.data_bytes,
             source.data_bytes,
             progress.duration_ms,
             esp_err_to_name(ret));
    return ret;
}

esp_err_t solar_os_audio_play_mp3(const char *path,
                                  uint8_t volume,
                                  const solar_os_audio_wav_options_t *options,
                                  solar_os_audio_wav_info_t *info)
{
    if (path == NULL || path[0] == '\0' || volume > 100) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    mp3dec_t *decoder = audio_heap_alloc(sizeof(*decoder));
    uint8_t *input = audio_heap_alloc(AUDIO_MP3_INPUT_BUFFER_BYTES);
    int16_t *decoded = audio_heap_alloc(sizeof(*decoded) * MINIMP3_MAX_SAMPLES_PER_FRAME);
    int16_t *output = audio_heap_alloc(sizeof(*output) * AUDIO_MP3_OUTPUT_SAMPLES_MAX);
    if (decoder == NULL || input == NULL || decoded == NULL || output == NULL) {
        fclose(file);
        heap_caps_free(decoder);
        heap_caps_free(input);
        heap_caps_free(decoded);
        heap_caps_free(output);
        return ESP_ERR_NO_MEM;
    }

    bool cancelled = false;
    solar_os_audio_wav_info_t progress;
    audio_mp3_fill_native_info(&progress, 0);
    esp_err_t ret = audio_mp3_seek_payload(file);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    ret = solar_os_audio_set_volume(volume);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    mp3dec_init(decoder);
    size_t input_len = 0;
    bool eof = false;
    bool decoded_any = false;
    audio_mp3_resampler_t resampler = {0};
    const uint32_t progress_interval_ms = audio_wav_progress_interval_ms(options);
    int64_t next_progress_us = esp_timer_get_time() + ((int64_t)progress_interval_ms * 1000);

    while (true) {
        if (audio_wav_should_cancel(options)) {
            cancelled = true;
            ret = ESP_ERR_TIMEOUT;
            break;
        }

        ret = audio_mp3_fill_input(file, input, &input_len, &eof);
        if (ret != ESP_OK) {
            break;
        }
        if (input_len == 0 && eof) {
            ret = decoded_any ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
            break;
        }

        mp3dec_frame_info_t frame = {0};
        const int samples = mp3dec_decode_frame(decoder,
                                                input,
                                                (int)input_len,
                                                decoded,
                                                &frame);
        if (samples > 0) {
            if (frame.hz < (int)AUDIO_MP3_OUTPUT_MIN_SAMPLE_RATE ||
                frame.channels <= 0 ||
                frame.channels > 2) {
                ret = ESP_ERR_NOT_SUPPORTED;
                break;
            }

            const size_t out_samples = audio_mp3_convert_to_native(decoded,
                                                                   samples,
                                                                   frame.channels,
                                                                   frame.hz,
                                                                   &resampler,
                                                                   output,
                                                                   AUDIO_MP3_OUTPUT_SAMPLES_MAX);
            if (out_samples == 0 || (out_samples % AUDIO_CODEC_BOARD_DEFAULT_CHANNELS) != 0) {
                ret = ESP_ERR_INVALID_RESPONSE;
                break;
            }

            const size_t out_bytes = out_samples * sizeof(output[0]);
            ret = audio_codec_board_write(output, out_bytes);
            if (ret != ESP_OK) {
                break;
            }

            decoded_any = true;
            progress.data_bytes += (uint32_t)out_bytes;
            progress.duration_ms =
                (uint32_t)((((uint64_t)progress.data_bytes / progress.block_align) * 1000U) /
                           progress.sample_rate);

            const int64_t now_us = esp_timer_get_time();
            if (now_us >= next_progress_us) {
                audio_wav_report_progress(options, &progress, false, false);
                next_progress_us = now_us + ((int64_t)progress_interval_ms * 1000);
            }
        }

        size_t consumed = frame.frame_bytes > 0 ? (size_t)frame.frame_bytes : 0;
        if (consumed > input_len) {
            consumed = input_len;
        }
        if (consumed == 0) {
            if (!eof && input_len < AUDIO_MP3_INPUT_BUFFER_BYTES) {
                continue;
            }
            consumed = input_len > 0 ? 1U : 0U;
        }
        audio_mp3_consume_input(input, &input_len, consumed);
    }

    memset(output, 0, AUDIO_WAV_BUFFER_BYTES);
    (void)audio_codec_board_write(output, AUDIO_WAV_BUFFER_BYTES);

cleanup:
    {
        const int close_errno = errno;
        fclose(file);
        errno = close_errno;
    }
    heap_caps_free(decoder);
    heap_caps_free(input);
    heap_caps_free(decoded);
    heap_caps_free(output);

    if (info != NULL) {
        *info = progress;
    }
    audio_wav_report_progress(options, &progress, true, cancelled);
    SOLAR_OS_LOGI(TAG,
             "play mp3 %s: bytes=%" PRIu32 " ms=%" PRIu32 " ret=%s",
             path,
             progress.data_bytes,
             progress.duration_ms,
             esp_err_to_name(ret));
    return ret;
}

void solar_os_audio_get_status(solar_os_audio_status_t *status)
{
    if (status == NULL) {
        return;
    }

    audio_codec_board_status_t board_status;
    audio_codec_board_get_status(&board_status);

    status->initialized = board_status.initialized;
    status->sample_rate = board_status.sample_rate;
    status->channels = board_status.channels;
    status->bits_per_sample = board_status.bits_per_sample;
    status->volume = board_status.volume;
    status->mic_gain_db = board_status.mic_gain_db;
    status->i2s_port = board_status.i2s_port;
    status->mclk_pin = board_status.mclk_pin;
    status->bclk_pin = board_status.bclk_pin;
    status->ws_pin = board_status.ws_pin;
    status->din_pin = board_status.din_pin;
    status->dout_pin = board_status.dout_pin;
    status->pa_pin = board_status.pa_pin;
    status->output_codec = board_status.output_codec;
    status->input_codec = board_status.input_codec;
}
