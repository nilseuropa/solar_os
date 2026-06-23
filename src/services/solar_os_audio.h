#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_AUDIO_TONE_MIN_HZ 20U
#define SOLAR_OS_AUDIO_TONE_MAX_HZ 8000U
#define SOLAR_OS_AUDIO_TEST_MAX_MS 10000U
#define SOLAR_OS_AUDIO_WAV_MAX_MS (60U * 60U * 1000U)
#define SOLAR_OS_AUDIO_WAV_DEFAULT_PROGRESS_MS 1000U

typedef struct {
    bool initialized;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t volume;
    float mic_gain_db;
    int i2s_port;
    int mclk_pin;
    int bclk_pin;
    int ws_pin;
    int din_pin;
    int dout_pin;
    int pa_pin;
    const char *output_codec;
    const char *input_codec;
} solar_os_audio_status_t;

typedef struct {
    uint32_t samples;
    uint8_t peak_percent;
    uint8_t average_percent;
} solar_os_audio_level_t;

typedef struct {
    uint32_t sample_rate;
    uint32_t data_bytes;
    uint32_t duration_ms;
    uint16_t block_align;
    uint8_t channels;
    uint8_t bits_per_sample;
} solar_os_audio_wav_info_t;

typedef struct {
    solar_os_audio_wav_info_t info;
    bool done;
    bool cancelled;
} solar_os_audio_wav_progress_t;

typedef bool (*solar_os_audio_wav_cancel_cb_t)(void *user);
typedef void (*solar_os_audio_wav_progress_cb_t)(const solar_os_audio_wav_progress_t *progress,
                                                 void *user);

typedef struct {
    solar_os_audio_wav_cancel_cb_t should_cancel;
    solar_os_audio_wav_progress_cb_t progress;
    void *user;
    uint32_t progress_interval_ms;
} solar_os_audio_wav_options_t;

esp_err_t solar_os_audio_init(void);
void solar_os_audio_deinit(void);
esp_err_t solar_os_audio_set_volume(uint8_t volume);
esp_err_t solar_os_audio_set_mic_gain(float gain_db);
esp_err_t solar_os_audio_play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume);
esp_err_t solar_os_audio_measure_level(uint32_t duration_ms, solar_os_audio_level_t *level);
esp_err_t solar_os_audio_loopback(uint32_t duration_ms, uint8_t volume);
esp_err_t solar_os_audio_get_wav_info(const char *path, solar_os_audio_wav_info_t *info);
esp_err_t solar_os_audio_get_mp3_info(const char *path, solar_os_audio_wav_info_t *info);
esp_err_t solar_os_audio_record_wav(const char *path,
                                    uint32_t duration_ms,
                                    const solar_os_audio_wav_options_t *options,
                                    solar_os_audio_wav_info_t *info);
esp_err_t solar_os_audio_play_wav(const char *path,
                                  uint8_t volume,
                                  const solar_os_audio_wav_options_t *options,
                                  solar_os_audio_wav_info_t *info);
esp_err_t solar_os_audio_play_mp3(const char *path,
                                  uint8_t volume,
                                  const solar_os_audio_wav_options_t *options,
                                  solar_os_audio_wav_info_t *info);
void solar_os_audio_get_status(solar_os_audio_status_t *status);
