#pragma once

#include "solar_os_stream.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
#include "solar_os_rtsp_client.h"
#endif

#define SOLAR_OS_SCRIPT_MEDIA_STREAMS 4U
#define SOLAR_OS_SCRIPT_MEDIA_TRANSFER_MAX 16384U
#define SOLAR_OS_SCRIPT_MEDIA_TIMEOUT_MAX 60000U
#define SOLAR_OS_SCRIPT_MEDIA_FRAME_MAX (512U * 1024U)

typedef struct solar_os_script_media solar_os_script_media_t;
typedef bool (*solar_os_script_media_cancel_fn)(void *user);
typedef struct {
    solar_os_stream_video_frame_t jpeg;
    bool network;
    uint32_t rtp_timestamp;
    uint64_t arrived_us;
} solar_os_script_media_frame_t;
bool solar_os_script_media_option_valid(solar_os_stream_type_t type, const char *key);

/* One caller task (interpreter or native job) owns a session. IDs are never pointers, cannot cross
 * sessions, and do not become valid again after close/release. Native workers
 * never call interpreters. Session allocation is lazy and PSRAM-only. */
esp_err_t solar_os_script_media_create(const char *owner,
    solar_os_script_media_cancel_fn cancel, void *user,
    solar_os_script_media_t **out);
void solar_os_script_media_destroy(solar_os_script_media_t *session);
esp_err_t solar_os_script_media_open(solar_os_script_media_t *session,
    const char *id, const solar_os_stream_open_options_t *options, uint32_t *handle);
esp_err_t solar_os_script_media_close(solar_os_script_media_t *session, uint32_t handle);
esp_err_t solar_os_script_media_close_all(solar_os_script_media_t *session);
esp_err_t solar_os_script_media_stream_info(solar_os_script_media_t *session,
    uint32_t handle, solar_os_stream_info_t *info);
esp_err_t solar_os_script_media_read(solar_os_script_media_t *session,
    uint32_t handle, void *data, size_t size, uint32_t timeout_ms, size_t *length);
esp_err_t solar_os_script_media_write(solar_os_script_media_t *session,
    uint32_t handle, const void *data, size_t size, uint32_t timeout_ms, size_t *length);
esp_err_t solar_os_script_media_scalar(solar_os_script_media_t *session,
    uint32_t handle, const solar_os_stream_read_options_t *options, float *value);
esp_err_t solar_os_script_media_acquire(solar_os_script_media_t *session,
    uint32_t handle, uint32_t *frame);
esp_err_t solar_os_script_media_frame(solar_os_script_media_t *session,
    uint32_t frame, solar_os_script_media_frame_t *info);
esp_err_t solar_os_script_media_release(solar_os_script_media_t *session, uint32_t frame);
/* Optional file_errno is reset on entry and receives the first filesystem
 * failure, including buffered-write failures during close. It survives any
 * later frame/source cleanup; non-filesystem errors leave it zero. */
esp_err_t solar_os_script_media_save(solar_os_script_media_t *session,
    uint32_t frame, const char *path, int *file_errno);
/* Snapshot owns a temporary source; release(frame) also closes that source. */
esp_err_t solar_os_script_media_snapshot(solar_os_script_media_t *session,
    const char *source, uint16_t width, uint16_t height, uint8_t quality, uint32_t *frame);

#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
esp_err_t solar_os_script_media_rtsp_open(solar_os_script_media_t *session,
    const char *url, bool video, bool audio, uint32_t *handle);
esp_err_t solar_os_script_media_rtsp_status(solar_os_script_media_t *session,
    uint32_t handle, solar_os_rtsp_client_status_t *status, bool *ended);
/* ESP_OK with frame=0 means no complete video frame within timeout. Audio is
 * played natively; video is a single compressed frame with drop-while-leased. */
esp_err_t solar_os_script_media_rtsp_read(solar_os_script_media_t *session,
    uint32_t handle, uint32_t timeout_ms, uint32_t *frame);
esp_err_t solar_os_script_media_rtsp_lateness(solar_os_script_media_t *session,
    uint32_t handle, uint32_t frame, int64_t *lateness_us);
#endif
