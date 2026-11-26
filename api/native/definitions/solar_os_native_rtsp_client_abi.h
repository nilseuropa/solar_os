#pragma once

#include "solar_os_native_media_abi.h"
#include "solar_os_native_media_protocol_abi.h"

#define SOLAR_OS_NATIVE_RTSP_CLIENT_SERVICE "media.rtsp.client"
#define SOLAR_OS_NATIVE_RTSP_CLIENT_ABI 1U

typedef struct {
    bool video, audio, diagnostics, use_volume;
    uint8_t reconnect_attempts, volume;
} solar_os_native_rtsp_options_v1_t;

typedef struct {
    void (*samples)(const int16_t *samples, size_t count, uint8_t channels, void *user);
    /* NULL on detach. Inspect the named output through media.audio.routes. */
    void (*device)(const char *id, void *user);
    void *user;
} solar_os_native_rtsp_callbacks_v1_t;

typedef struct {
    solar_os_rtp_jpeg_frame_t jpeg;
    uint64_t arrived_us;
    uint32_t epoch;
} solar_os_native_rtsp_video_v1_t;

typedef int (*solar_os_native_rtsp_video_fn)(const solar_os_native_rtsp_video_v1_t *frame, void *user);

/* Open creates a single-use RTSP client. Run on a network worker, then close
 * after run and visits have returned. Cancel and status may run concurrently
 * with its worker. Close returns BUSY while any operation is active.
 *
 * Audio callbacks run on the audio owner with borrowed PCM and device IDs.
 * Callback code and userdata must stay live through run, which joins its
 * audio owner before returning. Visit a video frame on its consuming task.
 * Frame bytes are borrowed for that call; copy them for deferred decoding.
 * The epoch identifies the connection that supplied the frame. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*open)(const char *url, const solar_os_native_rtsp_options_v1_t *options, solar_os_native_handle_t *client);
    int (*run)(solar_os_native_handle_t client, const solar_os_native_rtsp_callbacks_v1_t *callbacks);
    int (*cancel)(solar_os_native_handle_t client);
    int (*set_paused)(solar_os_native_handle_t client, bool paused);
    int (*status)(solar_os_native_handle_t client, solar_os_rtsp_client_status_t *status);
    int (*visit_video)(solar_os_native_handle_t client, solar_os_native_rtsp_video_fn visit, void *user, bool *received);
    int (*video_lateness)(solar_os_native_handle_t client, uint32_t timestamp, uint64_t arrived_us, int64_t *lateness_us);
    int (*close)(solar_os_native_handle_t client);
} solar_os_native_rtsp_client_api_v1_t;
