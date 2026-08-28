#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_config.h"
#include "solar_os_stream.h"

#define SOLAR_OS_AUDIO_TONE_MIN_HZ 20U
#define SOLAR_OS_AUDIO_TONE_MAX_HZ 8000U
#define SOLAR_OS_AUDIO_TEST_MAX_MS 10000U
#define SOLAR_OS_AUDIO_WAV_MAX_MS (60U * 60U * 1000U)
#define SOLAR_OS_AUDIO_WAV_DEFAULT_PROGRESS_MS 1000U
#define SOLAR_OS_AUDIO_VOLUME_GLOBAL 255U
#define SOLAR_OS_AUDIO_TONE_SEQUENCE_MAX_STEPS 8U
#define SOLAR_OS_AUDIO_TONE_SEQUENCE_MAX_MS 10000U
#define SOLAR_OS_AUDIO_TONE_QUEUE_CAPACITY 8U
#define SOLAR_OS_AUDIO_STREAM_OWNER_MAX 24U
#define SOLAR_OS_AUDIO_DEVICE_MAX 4U
#define SOLAR_OS_AUDIO_DEVICE_ID_MAX 16U
#define SOLAR_OS_AUDIO_DEVICE_NAME_MAX 40U
#define SOLAR_OS_AUDIO_CAPTURE_MAX_FRAMES 4096U
#define SOLAR_OS_AUDIO_CAPTURE_MAX_CHANNELS 2U

typedef enum {
    SOLAR_OS_AUDIO_DEVICE_CAP_INPUT = 1U << 0,
    SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT = 1U << 1,
    SOLAR_OS_AUDIO_DEVICE_CAP_VOLUME = 1U << 2,
    SOLAR_OS_AUDIO_DEVICE_CAP_INPUT_GAIN = 1U << 3,
} solar_os_audio_device_capability_t;

typedef struct solar_os_audio_stream solar_os_audio_stream_t;
typedef struct solar_os_audio_input_stream solar_os_audio_input_stream_t;

typedef solar_os_stream_audio_format_t solar_os_audio_stream_format_t;

typedef struct {
    char id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    char name[SOLAR_OS_AUDIO_DEVICE_NAME_MAX];
    char provider[SOLAR_OS_STREAM_PROVIDER_MAX];
    uint32_t capabilities;
    char capture_stream[SOLAR_OS_STREAM_ID_MAX];
    char playback_stream[SOLAR_OS_STREAM_ID_MAX];
    solar_os_audio_stream_format_t native_format;
    float input_gain_min_db;
    float input_gain_max_db;
    float input_gain_step_db;
} solar_os_audio_device_info_t;

typedef struct {
    /* Callbacks remain valid until the device is unregistered. */
    esp_err_t (*set_volume)(void *user, uint8_t volume);
    esp_err_t (*set_input_gain)(void *user, float gain_db);
    esp_err_t (*get_input_gain)(void *user, float *gain_db);
} solar_os_audio_device_ops_t;

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
    uint32_t frequency_hz;
    uint32_t duration_ms;
    uint32_t pause_ms;
} solar_os_audio_tone_step_t;

typedef struct {
    /* The service copies the steps before this call returns. */
    const solar_os_audio_tone_step_t *steps;
    size_t step_count;
    uint8_t volume;
    /* Drop instead of waiting when another audio operation is active. */
    bool drop_if_busy;
} solar_os_audio_tone_request_t;

typedef struct {
    bool worker_running;
    bool playing;
    size_t queued;
    uint32_t current_id;
    uint32_t completed;
    uint32_t cancelled;
    uint32_t dropped;
    uint32_t failed;
} solar_os_audio_tone_queue_status_t;

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
typedef bool (*solar_os_audio_wav_pause_cb_t)(void *user);
typedef bool (*solar_os_audio_wav_monitor_cb_t)(void *user);
typedef void (*solar_os_audio_wav_samples_cb_t)(const int16_t *samples,
                                                size_t sample_count,
                                                uint8_t channels,
                                                void *user);
typedef void (*solar_os_audio_wav_device_cb_t)(
    const solar_os_audio_device_info_t *device,
    void *user);
typedef void (*solar_os_audio_wav_progress_cb_t)(const solar_os_audio_wav_progress_t *progress,
                                                 void *user);

typedef struct {
    /* Playback stream owner; NULL keeps the aplay compatibility name. */
    const char *owner;
    /* Recording source; NULL selects the first compatible capture device. */
    const char *capture_stream;
    /* Zero fields retain the capture stream's native WAV format. */
    solar_os_audio_stream_format_t record_format;
    bool monitor;
    uint8_t monitor_volume;
    /* Optional live monitor control; when present it supersedes monitor. */
    solar_os_audio_wav_monitor_cb_t should_monitor;
    solar_os_audio_wav_cancel_cb_t should_cancel;
    solar_os_audio_wav_progress_cb_t progress;
    solar_os_audio_wav_pause_cb_t should_pause;
    solar_os_audio_wav_samples_cb_t samples;
    solar_os_audio_wav_device_cb_t device;
    void *user;
    uint32_t progress_interval_ms;
} solar_os_audio_wav_options_t;

esp_err_t solar_os_audio_init(void);
void solar_os_audio_deinit(void);
esp_err_t solar_os_audio_register_device(const solar_os_audio_device_info_t *device);
esp_err_t solar_os_audio_register_device_ex(
    const solar_os_audio_device_info_t *device,
    const solar_os_audio_device_ops_t *ops,
    void *user);
esp_err_t solar_os_audio_unregister_device(const char *id);
size_t solar_os_audio_device_count(void);
bool solar_os_audio_device_get(size_t index, solar_os_audio_device_info_t *device);
esp_err_t solar_os_audio_device_get_info(const char *id,
                                         solar_os_audio_device_info_t *device);
bool solar_os_audio_output_available(void);
/* An empty ID restores automatic first-compatible-device selection. */
esp_err_t solar_os_audio_set_default_output(const char *id);
bool solar_os_audio_get_default_output(char *id, size_t id_len);
/* Open the preferred endpoint, then the first other compatible endpoint. */
esp_err_t solar_os_audio_open_default(
    solar_os_stream_direction_t direction,
    const char *owner,
    const solar_os_stream_open_options_t *options,
    solar_os_stream_handle_t *stream,
    solar_os_audio_device_info_t *device);
esp_err_t solar_os_audio_set_device_volume(const char *id, uint8_t volume);
esp_err_t solar_os_audio_set_device_input_gain(const char *id, float gain_db);
esp_err_t solar_os_audio_get_device_input_gain(const char *id, float *gain_db);
/* Register the board device and its scalar/audio endpoints without powering it. */
esp_err_t solar_os_audio_register_streams(void);
esp_err_t solar_os_audio_unregister_streams(const char *id);
esp_err_t solar_os_audio_set_volume(uint8_t volume);
esp_err_t solar_os_audio_toggle_mute(uint8_t *volume_after);
esp_err_t solar_os_audio_set_mic_gain(float gain_db);
/*
 * Claim the output until solar_os_audio_stream_close(). The caller that opens
 * the stream must also write and close it. Samples use the native board PCM
 * format returned through format. A timeout of UINT32_MAX waits indefinitely.
 */
esp_err_t solar_os_audio_stream_open(const char *owner,
                                     uint32_t timeout_ms,
                                     solar_os_audio_stream_t **stream,
                                     solar_os_audio_stream_format_t *format);
esp_err_t solar_os_audio_stream_write(solar_os_audio_stream_t *stream,
                                      const void *data,
                                      size_t len);
void solar_os_audio_stream_close(solar_os_audio_stream_t *stream);
esp_err_t solar_os_audio_input_stream_open(const char *owner,
                                           uint32_t timeout_ms,
                                           solar_os_audio_input_stream_t **stream,
                                           solar_os_audio_stream_format_t *format);
esp_err_t solar_os_audio_input_stream_read(solar_os_audio_input_stream_t *stream,
                                           void *data,
                                           size_t len);
void solar_os_audio_input_stream_close(solar_os_audio_input_stream_t *stream);
/* Capture exactly frames of interleaved native S16LE PCM from the default input. */
esp_err_t solar_os_audio_capture(const char *owner,
                                 size_t frames,
                                 int16_t *samples,
                                 size_t sample_capacity,
                                 solar_os_audio_stream_format_t *format);
esp_err_t solar_os_audio_play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume);
esp_err_t solar_os_audio_tone_enqueue(const solar_os_audio_tone_request_t *request,
                                      uint32_t *request_id);
esp_err_t solar_os_audio_tone_cancel(uint32_t request_id);
void solar_os_audio_tone_queue_get_status(solar_os_audio_tone_queue_status_t *status);
esp_err_t solar_os_audio_measure_level(uint32_t duration_ms, solar_os_audio_level_t *level);
esp_err_t solar_os_audio_measure_channel_level(uint8_t channel,
                                               uint32_t duration_ms,
                                               solar_os_audio_level_t *level);
esp_err_t solar_os_audio_loopback(uint32_t duration_ms, uint8_t volume);
esp_err_t solar_os_audio_get_wav_info(const char *path, solar_os_audio_wav_info_t *info);
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO_CODECS
esp_err_t solar_os_audio_get_mp3_info(const char *path, solar_os_audio_wav_info_t *info);
#endif
/* A zero duration records until cancellation or the output cannot grow. */
esp_err_t solar_os_audio_record_wav(const char *path,
                                    uint32_t duration_ms,
                                    const solar_os_audio_wav_options_t *options,
                                    solar_os_audio_wav_info_t *info);
/* Route a selected capture stream to the default output until cancellation. */
esp_err_t solar_os_audio_monitor_stream(
    uint8_t volume,
    const solar_os_audio_wav_options_t *options,
    solar_os_audio_wav_info_t *info);
esp_err_t solar_os_audio_play_wav(const char *path,
                                  uint8_t volume,
                                  const solar_os_audio_wav_options_t *options,
                                  solar_os_audio_wav_info_t *info);
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO_CODECS
esp_err_t solar_os_audio_play_mp3(const char *path,
                                  uint8_t volume,
                                  const solar_os_audio_wav_options_t *options,
                                  solar_os_audio_wav_info_t *info);
#endif
void solar_os_audio_get_status(solar_os_audio_status_t *status);
