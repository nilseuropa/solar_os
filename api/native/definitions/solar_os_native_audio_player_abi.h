#pragma once

#include "solar_os_native_audio_routes_abi.h"

#define SOLAR_OS_NATIVE_AUDIO_PLAYER_SERVICE "media.audio.player"
#define SOLAR_OS_NATIVE_AUDIO_PLAYER_ABI 1U
#define SOLAR_OS_NATIVE_AUDIO_PLAYER_VOLUME_GLOBAL 255U

typedef struct {
    const char *owner;
    solar_os_native_stream_format_v1_t format;
    size_t external_buffer_bytes, internal_buffer_bytes;
    uint32_t target_ms, open_timeout_ms;
    uint8_t volume;
    bool buffered;
    /* Provide a library lease when using callbacks. The service retains
     * the code until close succeeds. Callback state remains caller-owned.
     * Buffered callbacks may run on the sink task and must return promptly. */
    solar_os_native_handle_t library_lease;
    void *user;
    void (*state)(bool playing, void *user);
    void (*samples)(const int16_t *samples, size_t sample_count, uint8_t channels, void *user);
    bool (*cancel)(void *user);
    bool (*pause)(void *user);
} solar_os_native_audio_player_options_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    /* Format fields may be zero to accept native defaults. open_timeout_ms
     * must be nonzero. Output format and device ID are optional copies.
     * Direct players perform I/O and close on the opening task. Errors
     * with a nonzero handle require close to finish cleanup. */
    int (*open)(const solar_os_native_audio_player_options_v1_t *options,
        solar_os_native_handle_t *player, solar_os_native_stream_format_v1_t *format,
        char device_id[SOLAR_OS_AUDIO_DEVICE_ID_MAX]);
    /* Write a whole number of interleaved signed 16-bit PCM frames. */
    int (*write)(solar_os_native_handle_t player, const int16_t *samples, size_t sample_count);
    int (*end_input)(solar_os_native_handle_t player);
    int (*finish)(solar_os_native_handle_t player);
    int (*error)(solar_os_native_handle_t player);
    /* A busy or failed close leaves the handle valid for another attempt. */
    int (*close)(solar_os_native_handle_t player);
} solar_os_native_audio_player_api_v1_t;
