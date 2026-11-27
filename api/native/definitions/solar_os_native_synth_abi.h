#pragma once

#include "solar_os_native_service_abi.h"
#include "solar_os_synth_types_v1.h"

#define SOLAR_OS_NATIVE_SYNTH_SERVICE "audio.synth"
#define SOLAR_OS_NATIVE_SYNTH_ABI 1U
#define SOLAR_OS_NATIVE_VOICE_SERVICE "audio.voice"
#define SOLAR_OS_NATIVE_VOICE_ABI 1U

typedef struct {
    const char *playback_stream;
    solar_os_synth_render_cb_t render;
    void *user;
    size_t block_frames;
    solar_os_native_handle_t library_lease;
} solar_os_native_synth_config_v1_t;

/* One renderer uses the shared synth output at a time. The callback fills
 * interleaved stereo PCM and runs on the synth worker. A library lease pins
 * its code. A zero lease requires an attaching native driver. The worker
 * uses the default output if playback_stream is empty or NULL. If open
 * returns an error with a nonzero handle, cleanup is pending. Keep the
 * callback context alive and retry close until it succeeds. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*open)(const solar_os_native_synth_config_v1_t *config, solar_os_native_handle_t *session);
    int (*close)(solar_os_native_handle_t session);
    int (*status)(solar_os_synth_status_t *status);
} solar_os_native_synth_api_v1_t;

/* Voices share one patch and one output. Open creates an owning handle.
 * The first note claims playback; editing while idle sets the next patch.
 * Close releases playback if the session acquired it. Samples are copied
 * immediately and buffers are caller-owned. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*open)(solar_os_native_handle_t *session);
    int (*close)(solar_os_native_handle_t session);
    int (*status)(solar_os_synth_voice_status_t *status);
    int (*configure)(solar_os_native_handle_t session, const solar_os_synth_voice_config_t *config);
    int (*configure_performance)(solar_os_native_handle_t session, const solar_os_synth_voice_performance_t *performance);
    int (*note_on)(solar_os_native_handle_t session, uint32_t frequency_hz, uint8_t velocity);
    int (*note_off)(solar_os_native_handle_t session, uint32_t frequency_hz);
    int (*all_notes_off)(solar_os_native_handle_t session);
    int (*set_wavetable)(solar_os_native_handle_t session, const int16_t *samples, size_t count);
    int (*get_wavetable)(int16_t *samples, size_t count);
    const char *(*waveform_name)(solar_os_synth_waveform_t waveform);
    bool (*parse_waveform)(const char *name, solar_os_synth_waveform_t *waveform);
} solar_os_native_voice_api_v1_t;
