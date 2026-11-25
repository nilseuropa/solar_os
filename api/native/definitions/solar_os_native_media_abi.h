#pragma once
#include "solar_os_native_service_abi.h"
#define SOLAR_OS_NATIVE_MEDIA_SERVICE "media.video"
#define SOLAR_OS_NATIVE_MEDIA_ABI 1U
#define SOLAR_OS_NATIVE_MEDIA_LOCAL 1U
#define SOLAR_OS_NATIVE_MEDIA_RTSP 2U
typedef struct solar_os_script_media solar_os_native_media_t;
typedef struct {
    uint16_t width, height;
    uint8_t jpeg_quality;
    uint32_t timeout_ms;
} solar_os_native_video_options_v1_t;
typedef struct {
    const uint8_t *data;
    size_t length;
    uint16_t width, height;
    uint64_t timestamp_us;
    bool network;
    uint32_t rtp_timestamp;
    uint64_t arrived_us;
} solar_os_native_video_frame_v1_t;

/* One calling task owns each session, serializing all calls on it. Source/frame
 * IDs are session-local, never pointers. frame() borrows JPEG bytes until
 * release_frame/close/destroy; image.decode copies them. destroy joins the RTSP
 * worker and releases all leases/sources. No module callback runs on that worker.
 * rtsp_read returns OK with frame=0 when no complete frame is ready. */
typedef struct {
    uint32_t abi_version, struct_size, capabilities;
    int (*create)(const char *owner, solar_os_native_cancel_fn cancel, void *user,
        solar_os_native_media_t **session);
    /* On provider close failure the session remains valid for retry. */
    int (*destroy)(solar_os_native_media_t *session);
    int (*open)(solar_os_native_media_t *session, const char *stream_id,
        const solar_os_native_video_options_v1_t *options, uint32_t *source);
    int (*close)(solar_os_native_media_t *session, uint32_t source);
    int (*acquire)(solar_os_native_media_t *session, uint32_t source, uint32_t *frame);
    int (*frame)(solar_os_native_media_t *session, uint32_t frame,
        solar_os_native_video_frame_v1_t *view);
    int (*release_frame)(solar_os_native_media_t *session, uint32_t frame);
    int (*snapshot)(solar_os_native_media_t *session, const char *stream_id,
        uint16_t width, uint16_t height, uint8_t jpeg_quality, uint32_t *frame);
    /* Without RTSP capability these three functions return NOT_SUPPORTED. */
    int (*rtsp_open)(solar_os_native_media_t *session, const char *url, uint32_t *source);
    int (*rtsp_read)(solar_os_native_media_t *session, uint32_t source,
        uint32_t timeout_ms, uint32_t *frame);
    int (*rtsp_ended)(solar_os_native_media_t *session, uint32_t source, bool *ended);
} solar_os_native_media_api_v1_t;
