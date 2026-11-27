#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_AUDIO_SERVICE "media.audio"
#define SOLAR_OS_NATIVE_AUDIO_ABI 1U
#define SOLAR_OS_AUDIO_FORMAT_AUTO 0U
#define SOLAR_OS_AUDIO_FORMAT_WAV 1U
#define SOLAR_OS_AUDIO_FORMAT_MP3 2U
#define SOLAR_OS_AUDIO_FILE_VOLUME_GLOBAL 255U
#define SOLAR_OS_AUDIO_FILE_CAP_WAV (1U << 0)
#define SOLAR_OS_AUDIO_FILE_CAP_MP3 (1U << 1)

#define SOLAR_OS_AUDIO_TONE_MIN_HZ 20U
#define SOLAR_OS_AUDIO_TONE_MAX_HZ 8000U
#define SOLAR_OS_AUDIO_TONE_SEQUENCE_MAX_STEPS 8U
#define SOLAR_OS_AUDIO_TONE_SEQUENCE_MAX_MS 10000U
#define SOLAR_OS_AUDIO_TONE_QUEUE_CAPACITY 8U

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
    uint32_t sample_rate, data_bytes, duration_ms;
    uint16_t block_align;
    uint8_t channels, bits_per_sample;
} solar_os_audio_file_info_v1_t;

typedef struct {
    solar_os_audio_file_info_v1_t info;
    bool done, cancelled;
} solar_os_audio_file_progress_v1_t;

typedef struct {
    const char *owner;
    const char *capture_stream;
    uint32_t sample_rate;
    uint32_t progress_interval_ms, start_ms;
    uint8_t channels, monitor_volume;
    bool monitor;
    solar_os_native_cancel_fn should_cancel, should_pause, should_monitor;
    void (*progress)(const solar_os_audio_file_progress_v1_t *progress, void *user);
    /* sample_count counts interleaved signed 16-bit samples. */
    void (*samples)(const int16_t *samples, size_t sample_count, uint8_t channels, void *user);
    /* Reports capture and playback routes when they open.
     * Missing metadata is represented by an empty device ID. */
    void (*device)(bool capture, const char *id, const char *capture_stream,
        const char *playback_stream, void *user);
    void *user;
} solar_os_audio_file_options_v1_t;

/* Blocking operations share Core stream selection and buffering. Callback
 * arguments are borrowed during the call. Callbacks run on the caller's task
 * and are never retained. Use sample_rate/channels of zero for native capture
 * PCM. The stream and parameters services expose endpoints and their controls.
 * Probe takes an explicit format or AUTO, and writes the detected format.
 * Play accepts the format returned by probe. Record writes a WAV file, with
 * zero duration allowing recording until cancelled or storage is exhausted. */
typedef struct {
    uint32_t abi_version, struct_size, capabilities;
    int (*probe)(const char *path, uint32_t format, uint32_t *detected,
        solar_os_audio_file_info_v1_t *info);
    int (*play)(const char *path, uint32_t format, uint8_t volume,
        const solar_os_audio_file_options_v1_t *options, solar_os_audio_file_info_v1_t *info);
    int (*record)(const char *path, uint32_t duration_ms,
        const solar_os_audio_file_options_v1_t *options, solar_os_audio_file_info_v1_t *info);
    int (*monitor)(uint8_t volume, const solar_os_audio_file_options_v1_t *options,
        solar_os_audio_file_info_v1_t *info);
    /* Play uses the selected audio output and blocks through the tone.
     * Enqueue copies the sequence. Its optional request ID is valid while
     * queued or playing and can be cancelled from another task. */
    int (*play_tone)(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume);
    int (*enqueue_tone)(const solar_os_audio_tone_request_t *request, uint32_t *request_id);
    int (*cancel_tone)(uint32_t request_id);
    void (*tone_status)(solar_os_audio_tone_queue_status_t *status);
    /* Board audio lifecycle and a bounded input-to-output hardware test.
     * Use monitor for arbitrary registered capture and playback routes. */
    int (*prepare_board)(void);
    void (*release_idle_board)(void);
    int (*loopback)(uint32_t duration_ms, uint8_t volume);
} solar_os_native_audio_api_v1_t;
