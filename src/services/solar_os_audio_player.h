#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_audio.h"

#define SOLAR_OS_AUDIO_PLAYER_DEFAULT_BUFFER_BYTES (32U * 1024U)
#define SOLAR_OS_AUDIO_PLAYER_DEFAULT_TARGET_MS 500U

typedef struct solar_os_audio_player solar_os_audio_player_t;

typedef void (*solar_os_audio_player_state_cb_t)(bool playing, void *user);
typedef void (*solar_os_audio_player_samples_cb_t)(const int16_t *samples,
                                                   size_t sample_count,
                                                   uint8_t channels,
                                                   void *user);
typedef bool (*solar_os_audio_player_cancel_cb_t)(void *user);
typedef bool (*solar_os_audio_player_pause_cb_t)(void *user);

typedef struct {
    const char *owner;
    solar_os_stream_audio_format_t requested_audio;
    uint8_t volume;
    /* Buffered mode owns the stream from a dedicated sink task. */
    bool buffered;
    size_t external_buffer_bytes;
    size_t internal_buffer_bytes;
    uint32_t target_ms;
    /* Zero preserves the existing indefinite wait for audio ownership. */
    uint32_t open_timeout_ms;
    solar_os_audio_player_state_cb_t state;
    solar_os_audio_player_samples_cb_t samples;
    void *user;
    solar_os_audio_player_cancel_cb_t should_cancel;
    void *cancel_user;
    solar_os_audio_player_pause_cb_t should_pause;
    void *pause_user;
} solar_os_audio_player_options_t;

/*
 * Start the shared PCM sink used by finite players and streaming producers.
 * Buffered mode owns the audio stream from a dedicated sink task. Direct mode
 * keeps stream ownership in the caller's existing worker and adds no task or
 * jitter buffer.
 */
esp_err_t solar_os_audio_player_create(
    const solar_os_audio_player_options_t *options,
    solar_os_audio_player_t **player,
    solar_os_stream_audio_format_t *format,
    solar_os_audio_device_info_t *device);
esp_err_t solar_os_audio_player_write(solar_os_audio_player_t *player,
                                      const void *data,
                                      size_t len,
                                      const volatile bool *cancelled);
esp_err_t solar_os_audio_player_finish(solar_os_audio_player_t *player,
                                       const volatile bool *cancelled);
esp_err_t solar_os_audio_player_error(const solar_os_audio_player_t *player);
void solar_os_audio_player_destroy(solar_os_audio_player_t *player);
