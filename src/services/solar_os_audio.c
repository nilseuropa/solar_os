#include "solar_os_audio.h"

#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "solar_os_log.h"
#include "solar_os_board_audio.h"
#include "solar_os_board_caps.h"
#include "esp_timer.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_APP_APLAY
#include "minimp3.h"
#endif
#include "solar_os_storage.h"

#define AUDIO_FRAME_CHUNK 256U
#define AUDIO_LEVEL_BUFFER_SAMPLES 512U
#define AUDIO_LOOPBACK_BUFFER_BYTES 2048U
#define AUDIO_TONE_AMPLITUDE 12000
#define AUDIO_WAV_HEADER_BYTES 44U
#define AUDIO_WAV_BUFFER_BYTES 4096U
#define AUDIO_WAV_PCM_FORMAT 1U
#if SOLAR_OS_PACKAGE_APP_APLAY
#define AUDIO_MP3_INPUT_BUFFER_BYTES 16384U
#define AUDIO_MP3_PROBE_SCAN_BYTES 65536U
#define AUDIO_MP3_OUTPUT_MIN_SAMPLE_RATE 8000U
#define AUDIO_MP3_OUTPUT_SAMPLES_MAX (AUDIO_WAV_BUFFER_BYTES / sizeof(int16_t))
#endif

static const char *TAG = "solar_os_audio";
static uint8_t audio_global_volume = SOLAR_OS_BOARD_AUDIO_DEFAULT_VOLUME;
static uint8_t audio_mute_restore_volume = SOLAR_OS_BOARD_AUDIO_DEFAULT_VOLUME;

#if SOLAR_OS_PACKAGE_APP_APLAY
typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint64_t phase_q16;
} audio_mp3_resampler_t;
#endif

static uint8_t clamp_percent_u32(uint32_t value)
{
    return value > 100U ? 100U : (uint8_t)value;
}

static bool audio_volume_arg_valid(uint8_t volume)
{
    return volume <= 100U || volume == SOLAR_OS_AUDIO_VOLUME_GLOBAL;
}

static uint8_t audio_resolve_playback_volume(uint8_t volume)
{
    return volume == SOLAR_OS_AUDIO_VOLUME_GLOBAL ? audio_global_volume : volume;
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
    info->sample_rate = SOLAR_OS_BOARD_AUDIO_DEFAULT_SAMPLE_RATE;
    info->channels = SOLAR_OS_BOARD_AUDIO_DEFAULT_CHANNELS;
    info->bits_per_sample = SOLAR_OS_BOARD_AUDIO_DEFAULT_BITS;
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
        info->sample_rate == SOLAR_OS_BOARD_AUDIO_DEFAULT_SAMPLE_RATE &&
        info->channels == SOLAR_OS_BOARD_AUDIO_DEFAULT_CHANNELS &&
        info->bits_per_sample == SOLAR_OS_BOARD_AUDIO_DEFAULT_BITS &&
        info->block_align == ((SOLAR_OS_BOARD_AUDIO_DEFAULT_CHANNELS *
                               SOLAR_OS_BOARD_AUDIO_DEFAULT_BITS) / 8U);
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

#if SOLAR_OS_PACKAGE_APP_APLAY
static void *audio_heap_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void audio_log_heap_nomem(const char *where, size_t bytes)
{
    SOLAR_OS_LOGW(TAG,
                  "%s alloc %u failed: internal free=%u largest=%u psram free=%u largest=%u",
                  where != NULL ? where : "audio",
                  (unsigned)bytes,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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
                                          size_t output_samples_cap,
                                          bool *source_done)
{
    if (input == NULL || frames <= 0 || output == NULL || resampler == NULL ||
        channels <= 0 || channels > 2 || sample_rate <= 0) {
        if (source_done != NULL) {
            *source_done = false;
        }
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
    uint64_t step = ((uint64_t)sample_rate << 16) / SOLAR_OS_BOARD_AUDIO_DEFAULT_SAMPLE_RATE;
    if (step == 0) {
        step = 1;
    }

    size_t out_samples = 0;
    const size_t out_frame_cap = output_samples_cap / SOLAR_OS_BOARD_AUDIO_DEFAULT_CHANNELS;
    while (phase < limit && (out_samples / SOLAR_OS_BOARD_AUDIO_DEFAULT_CHANNELS) < out_frame_cap) {
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

    const bool done = phase >= limit;
    resampler->phase_q16 = done ? phase - limit : phase;
    if (source_done != NULL) {
        *source_done = done;
    }
    return out_samples;
}

static void audio_mp3_fill_native_info(solar_os_audio_wav_info_t *info, uint32_t data_bytes)
{
    audio_wav_fill_native_info(info, data_bytes);
}

static esp_err_t audio_mp3_playback_flush(int16_t *playback,
                                          size_t *playback_samples,
                                          bool pad_tail)
{
    if (playback == NULL || playback_samples == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*playback_samples == 0) {
        return ESP_OK;
    }

    if (pad_tail) {
        const size_t quantum = AUDIO_FRAME_CHUNK * SOLAR_OS_BOARD_AUDIO_DEFAULT_CHANNELS;
        size_t padded = ((*playback_samples + quantum - 1U) / quantum) * quantum;
        if (padded > AUDIO_MP3_OUTPUT_SAMPLES_MAX) {
            padded = AUDIO_MP3_OUTPUT_SAMPLES_MAX;
        }
        while (*playback_samples < padded) {
            playback[(*playback_samples)++] = 0;
        }
    }

    const esp_err_t ret = solar_os_board_audio_write(playback,
                                                     *playback_samples * sizeof(playback[0]));
    if (ret == ESP_OK) {
        *playback_samples = 0;
    }
    return ret;
}

static esp_err_t audio_mp3_playback_append(int16_t *playback,
                                           size_t *playback_samples,
                                           const int16_t *samples,
                                           size_t sample_count)
{
    if (playback == NULL || playback_samples == NULL || samples == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (sample_count > 0) {
        if (*playback_samples >= AUDIO_MP3_OUTPUT_SAMPLES_MAX) {
            const esp_err_t ret = audio_mp3_playback_flush(playback, playback_samples, false);
            if (ret != ESP_OK) {
                return ret;
            }
        }

        size_t space = AUDIO_MP3_OUTPUT_SAMPLES_MAX - *playback_samples;
        size_t chunk = sample_count < space ? sample_count : space;
        memcpy(playback + *playback_samples, samples, chunk * sizeof(samples[0]));
        *playback_samples += chunk;
        samples += chunk;
        sample_count -= chunk;

        if (*playback_samples >= AUDIO_MP3_OUTPUT_SAMPLES_MAX) {
            const esp_err_t ret = audio_mp3_playback_flush(playback, playback_samples, false);
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }

    return ESP_OK;
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
#endif

esp_err_t solar_os_audio_init(void)
{
#if !SOLAR_OS_BOARD_HAS_AUDIO
    return ESP_ERR_NOT_SUPPORTED;
#else
    return solar_os_board_audio_init();
#endif
}

void solar_os_audio_deinit(void)
{
#if SOLAR_OS_BOARD_HAS_AUDIO
    solar_os_board_audio_deinit();
#endif
}

esp_err_t solar_os_audio_set_volume(uint8_t volume)
{
#if !SOLAR_OS_BOARD_HAS_AUDIO
    (void)volume;
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = solar_os_board_audio_set_volume(volume);
    if (ret == ESP_OK) {
        audio_global_volume = volume;
        if (volume > 0) {
            audio_mute_restore_volume = volume;
        }
    }
    return ret;
#endif
}

static esp_err_t audio_apply_playback_volume(uint8_t volume)
{
#if !SOLAR_OS_BOARD_HAS_AUDIO
    (void)volume;
    return ESP_ERR_NOT_SUPPORTED;
#else
    return solar_os_board_audio_set_volume(audio_resolve_playback_volume(volume));
#endif
}

esp_err_t solar_os_audio_toggle_mute(uint8_t *volume_after)
{
#if !SOLAR_OS_BOARD_HAS_AUDIO
    if (volume_after != NULL) {
        *volume_after = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
#else
    uint8_t target = audio_mute_restore_volume;
    if (audio_global_volume > 0) {
        audio_mute_restore_volume = audio_global_volume;
        target = 0;
    } else if (target == 0 || target > 100) {
        target = SOLAR_OS_BOARD_AUDIO_DEFAULT_VOLUME;
    }

    const esp_err_t ret = solar_os_board_audio_set_volume(target);
    if (ret == ESP_OK) {
        audio_global_volume = target;
        if (target > 0) {
            audio_mute_restore_volume = target;
        }
        if (volume_after != NULL) {
            *volume_after = target;
        }
    }
    return ret;
#endif
}

esp_err_t solar_os_audio_set_mic_gain(float gain_db)
{
#if !SOLAR_OS_BOARD_HAS_AUDIO
    (void)gain_db;
    return ESP_ERR_NOT_SUPPORTED;
#else
    return solar_os_board_audio_set_mic_gain(gain_db);
#endif
}

esp_err_t solar_os_audio_play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume)
{
    if (frequency_hz < SOLAR_OS_AUDIO_TONE_MIN_HZ ||
        frequency_hz > SOLAR_OS_AUDIO_TONE_MAX_HZ ||
        duration_ms == 0 ||
        duration_ms > SOLAR_OS_AUDIO_TEST_MAX_MS ||
        !audio_volume_arg_valid(volume)) {
        return ESP_ERR_INVALID_ARG;
    }

#if !SOLAR_OS_BOARD_HAS_AUDIO
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = audio_apply_playback_volume(volume);
    if (ret != ESP_OK) {
        return ret;
    }

    solar_os_board_audio_status_t status;
    solar_os_board_audio_get_status(&status);

    int16_t samples[AUDIO_FRAME_CHUNK * SOLAR_OS_BOARD_AUDIO_DEFAULT_CHANNELS];
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
            for (uint8_t ch = 0; ch < SOLAR_OS_BOARD_AUDIO_DEFAULT_CHANNELS; ch++) {
                samples[(frame * SOLAR_OS_BOARD_AUDIO_DEFAULT_CHANNELS) + ch] = sample;
            }
        }

        ret = solar_os_board_audio_write(samples,
                                      frames * SOLAR_OS_BOARD_AUDIO_DEFAULT_CHANNELS * sizeof(samples[0]));
        if (ret != ESP_OK) {
            return ret;
        }
        frames_remaining -= frames;
    }

    memset(samples, 0, sizeof(samples));
    (void)solar_os_board_audio_write(samples, sizeof(samples));
    solar_os_board_audio_deinit();
    SOLAR_OS_LOGI(TAG, "tone: %" PRIu32 " Hz %" PRIu32 " ms vol=%u",
             frequency_hz,
             duration_ms,
             volume);
    return ESP_OK;
#endif
}

esp_err_t solar_os_audio_measure_level(uint32_t duration_ms, solar_os_audio_level_t *level)
{
    if (duration_ms == 0 || duration_ms > SOLAR_OS_AUDIO_TEST_MAX_MS || level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(level, 0, sizeof(*level));
#if !SOLAR_OS_BOARD_HAS_AUDIO
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = ESP_OK;

    int16_t samples[AUDIO_LEVEL_BUFFER_SAMPLES];
    const int64_t deadline_us = esp_timer_get_time() + ((int64_t)duration_ms * 1000);
    uint64_t sum_abs = 0;
    uint32_t peak = 0;

    while (esp_timer_get_time() < deadline_us) {
        ret = solar_os_board_audio_read(samples, sizeof(samples));
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
#endif
}

esp_err_t solar_os_audio_measure_channel_level(uint8_t channel,
                                               uint32_t duration_ms,
                                               solar_os_audio_level_t *level)
{
    if (duration_ms == 0 ||
        duration_ms > SOLAR_OS_AUDIO_TEST_MAX_MS ||
        level == NULL ||
        channel >= SOLAR_OS_BOARD_AUDIO_DEFAULT_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(level, 0, sizeof(*level));
#if !SOLAR_OS_BOARD_HAS_AUDIO
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = ESP_OK;

    int16_t samples[AUDIO_LEVEL_BUFFER_SAMPLES];
    const int64_t deadline_us = esp_timer_get_time() + ((int64_t)duration_ms * 1000);
    uint64_t sum_abs = 0;
    uint32_t peak = 0;

    while (esp_timer_get_time() < deadline_us) {
        ret = solar_os_board_audio_read(samples, sizeof(samples));
        if (ret != ESP_OK) {
            return ret;
        }

        for (size_t i = channel;
             i < sizeof(samples) / sizeof(samples[0]);
             i += SOLAR_OS_BOARD_AUDIO_DEFAULT_CHANNELS) {
            const uint32_t value = audio_abs_i16(samples[i]);
            if (value > peak) {
                peak = value;
            }
            sum_abs += value;
            level->samples++;
        }
    }

    if (level->samples == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint32_t average = (uint32_t)(sum_abs / level->samples);
    level->peak_percent = clamp_percent_u32((peak * 100U) / 32767U);
    level->average_percent = clamp_percent_u32((average * 100U) / 32767U);
    SOLAR_OS_LOGD(TAG,
             "level ch%u: samples=%" PRIu32 " peak=%u avg=%u",
             (unsigned)channel,
             level->samples,
             level->peak_percent,
             level->average_percent);
    return ESP_OK;
#endif
}

esp_err_t solar_os_audio_loopback(uint32_t duration_ms, uint8_t volume)
{
    if (duration_ms == 0 ||
        duration_ms > SOLAR_OS_AUDIO_TEST_MAX_MS ||
        !audio_volume_arg_valid(volume)) {
        return ESP_ERR_INVALID_ARG;
    }

#if !SOLAR_OS_BOARD_HAS_AUDIO
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = audio_apply_playback_volume(volume);
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
        ret = solar_os_board_audio_read(buffer, AUDIO_LOOPBACK_BUFFER_BYTES);
        if (ret != ESP_OK) {
            break;
        }
        ret = solar_os_board_audio_write(buffer, AUDIO_LOOPBACK_BUFFER_BYTES);
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
#endif
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

#if SOLAR_OS_PACKAGE_APP_APLAY
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
#endif

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
#if !SOLAR_OS_BOARD_HAS_AUDIO
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

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

        ret = solar_os_board_audio_read(buffer, chunk);
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
#endif
}

esp_err_t solar_os_audio_play_wav(const char *path,
                                  uint8_t volume,
                                  const solar_os_audio_wav_options_t *options,
                                  solar_os_audio_wav_info_t *info)
{
    if (path == NULL || path[0] == '\0' || !audio_volume_arg_valid(volume)) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_BOARD_HAS_AUDIO
    return ESP_ERR_NOT_SUPPORTED;
#else
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

    ret = audio_apply_playback_volume(volume);
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
        ret = solar_os_board_audio_write(buffer, chunk);
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

    if (ret == ESP_OK || cancelled) {
        memset(buffer, 0, AUDIO_WAV_BUFFER_BYTES);
        (void)solar_os_board_audio_write(buffer, AUDIO_WAV_BUFFER_BYTES);
    }
    solar_os_board_audio_deinit();

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
#endif
}

#if SOLAR_OS_PACKAGE_APP_APLAY
esp_err_t solar_os_audio_play_mp3(const char *path,
                                  uint8_t volume,
                                  const solar_os_audio_wav_options_t *options,
                                  solar_os_audio_wav_info_t *info)
{
    if (path == NULL || path[0] == '\0' || !audio_volume_arg_valid(volume)) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_BOARD_HAS_AUDIO
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = audio_mp3_seek_payload(file);
    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }

    ret = audio_apply_playback_volume(volume);
    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }

    mp3dec_t *decoder = audio_heap_alloc(sizeof(*decoder));
    uint8_t *input = audio_heap_alloc(AUDIO_MP3_INPUT_BUFFER_BYTES);
    int16_t *decoded = audio_heap_alloc(sizeof(*decoded) * MINIMP3_MAX_SAMPLES_PER_FRAME);
    int16_t *output = audio_heap_alloc(AUDIO_WAV_BUFFER_BYTES);
    int16_t *playback = audio_heap_alloc(AUDIO_WAV_BUFFER_BYTES);
    if (decoder == NULL || input == NULL || decoded == NULL || output == NULL || playback == NULL) {
        if (decoder == NULL) {
            audio_log_heap_nomem("mp3 decoder", sizeof(*decoder));
        }
        if (input == NULL) {
            audio_log_heap_nomem("mp3 input", AUDIO_MP3_INPUT_BUFFER_BYTES);
        }
        if (decoded == NULL) {
            audio_log_heap_nomem("mp3 decoded", sizeof(*decoded) * MINIMP3_MAX_SAMPLES_PER_FRAME);
        }
        if (output == NULL) {
            audio_log_heap_nomem("mp3 output", AUDIO_WAV_BUFFER_BYTES);
        }
        if (playback == NULL) {
            audio_log_heap_nomem("mp3 playback", AUDIO_WAV_BUFFER_BYTES);
        }
        fclose(file);
        heap_caps_free(decoder);
        heap_caps_free(input);
        heap_caps_free(decoded);
        heap_caps_free(output);
        heap_caps_free(playback);
        return ESP_ERR_NO_MEM;
    }

    bool cancelled = false;
    solar_os_audio_wav_info_t progress;
    audio_mp3_fill_native_info(&progress, 0);
    mp3dec_init(decoder);
    size_t input_len = 0;
    bool eof = false;
    bool decoded_any = false;
    size_t playback_samples = 0;
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

            decoded_any = true;
            bool source_done = false;
            do {
                if (audio_wav_should_cancel(options)) {
                    cancelled = true;
                    ret = ESP_ERR_TIMEOUT;
                    break;
                }

                const size_t out_samples = audio_mp3_convert_to_native(decoded,
                                                                       samples,
                                                                       frame.channels,
                                                                       frame.hz,
                                                                       &resampler,
                                                                       output,
                                                                       AUDIO_MP3_OUTPUT_SAMPLES_MAX,
                                                                       &source_done);
                if (out_samples == 0 ||
                    (out_samples % SOLAR_OS_BOARD_AUDIO_DEFAULT_CHANNELS) != 0) {
                    ret = ESP_ERR_INVALID_RESPONSE;
                    break;
                }

                const size_t out_bytes = out_samples * sizeof(output[0]);
                ret = audio_mp3_playback_append(playback,
                                                &playback_samples,
                                                output,
                                                out_samples);
                if (ret != ESP_OK) {
                    break;
                }

                progress.data_bytes += (uint32_t)out_bytes;
                progress.duration_ms =
                    (uint32_t)((((uint64_t)progress.data_bytes / progress.block_align) * 1000U) /
                               progress.sample_rate);
            } while (!source_done);

            if (ret != ESP_OK) {
                break;
            }

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

    if (ret == ESP_OK) {
        ret = audio_mp3_playback_flush(playback, &playback_samples, true);
    }

    if (ret == ESP_OK || cancelled) {
        memset(output, 0, AUDIO_WAV_BUFFER_BYTES);
        (void)solar_os_board_audio_write(output, AUDIO_WAV_BUFFER_BYTES);
    }
    solar_os_board_audio_deinit();

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
    heap_caps_free(playback);

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
#endif
}
#endif

void solar_os_audio_get_status(solar_os_audio_status_t *status)
{
    if (status == NULL) {
        return;
    }

#if !SOLAR_OS_BOARD_HAS_AUDIO
    *status = (solar_os_audio_status_t){
        .initialized = false,
        .sample_rate = SOLAR_OS_BOARD_AUDIO_DEFAULT_SAMPLE_RATE,
        .channels = SOLAR_OS_BOARD_AUDIO_DEFAULT_CHANNELS,
        .bits_per_sample = SOLAR_OS_BOARD_AUDIO_DEFAULT_BITS,
        .volume = 0,
        .mic_gain_db = 0.0f,
        .i2s_port = -1,
        .mclk_pin = -1,
        .bclk_pin = -1,
        .ws_pin = -1,
        .din_pin = -1,
        .dout_pin = -1,
        .pa_pin = -1,
        .output_codec = "-",
        .input_codec = "-",
    };
#else
    solar_os_board_audio_status_t board_status;
    solar_os_board_audio_get_status(&board_status);

    status->initialized = board_status.initialized;
    status->sample_rate = board_status.sample_rate;
    status->channels = board_status.channels;
    status->bits_per_sample = board_status.bits_per_sample;
    status->volume = audio_global_volume;
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
#endif
}
