#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_RTP_SERVICE "media.rtp"
#define SOLAR_OS_NATIVE_RTP_ABI 1U
#define SOLAR_OS_NATIVE_RTSP_PROTOCOL_SERVICE "media.rtsp.protocol"
#define SOLAR_OS_NATIVE_RTSP_PROTOCOL_ABI 1U

#define SOLAR_OS_MEDIA_RTP_PACKET_MAX 1200U
#define SOLAR_OS_MEDIA_VIDEO_FRAME_MAX (512U * 1024U)

typedef enum {
    SOLAR_OS_MEDIA_TRACK_VIDEO = 0,
    SOLAR_OS_MEDIA_TRACK_AUDIO,
} solar_os_media_track_kind_t;

typedef enum {
    SOLAR_OS_MEDIA_CODEC_JPEG = 0,
    SOLAR_OS_MEDIA_CODEC_L16,
} solar_os_media_codec_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t frame_rate;
} solar_os_media_video_format_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
} solar_os_media_audio_format_t;

typedef struct {
    solar_os_media_track_kind_t kind;
    solar_os_media_codec_t codec;
    uint8_t payload_type;
    uint32_t clock_rate;
    union {
        solar_os_media_video_format_t video;
        solar_os_media_audio_format_t audio;
    } format;
} solar_os_media_track_t;

typedef struct {
    uint64_t origin_us;
    uint32_t origin_timestamp;
    uint32_t clock_rate;
} solar_os_media_clock_t;

#define SOLAR_OS_RTP_HEADER_BYTES 12U

typedef struct {
    bool marker;
    uint8_t payload_type;
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
} solar_os_rtp_header_t;

typedef struct {
    uint8_t payload_type;
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
    size_t max_packet_bytes;
} solar_os_rtp_sender_t;

#define SOLAR_OS_RTP_JPEG_HEADER_BYTES 8U
#define SOLAR_OS_RTP_JPEG_HEADER_RESERVE 1024U
#define SOLAR_OS_RTP_JPEG_QUANT_BYTES 128U

typedef struct {
    const uint8_t *scan;
    size_t scan_len;
    uint16_t width;
    uint16_t height;
    uint16_t restart_interval;
    uint8_t type;
    uint8_t quant_tables[SOLAR_OS_RTP_JPEG_QUANT_BYTES];
} solar_os_rtp_jpeg_view_t;

typedef struct {
    const uint8_t *data;
    size_t length;
    uint16_t width;
    uint16_t height;
    uint32_t timestamp;
    uint32_t ssrc;
} solar_os_rtp_jpeg_frame_t;

#define SOLAR_OS_RTSP_URI_MAX 192U
#define SOLAR_OS_RTSP_SESSION_MAX 24U

typedef enum {
    SOLAR_OS_RTSP_METHOD_OPTIONS = 0,
    SOLAR_OS_RTSP_METHOD_DESCRIBE,
    SOLAR_OS_RTSP_METHOD_SETUP,
    SOLAR_OS_RTSP_METHOD_PLAY,
    SOLAR_OS_RTSP_METHOD_TEARDOWN,
    SOLAR_OS_RTSP_METHOD_GET_PARAMETER,
    SOLAR_OS_RTSP_METHOD_UNSUPPORTED,
} solar_os_rtsp_method_t;

typedef struct {
    solar_os_rtsp_method_t method;
    char uri[SOLAR_OS_RTSP_URI_MAX];
    uint32_t cseq;
    char session[SOLAR_OS_RTSP_SESSION_MAX];
    uint16_t client_rtp_port;
    uint16_t client_rtcp_port;
} solar_os_rtsp_request_t;

typedef int (*solar_os_native_rtp_packet_fn)(const uint8_t *packet, size_t bytes, void *user);
typedef int (*solar_os_native_rtp_jpeg_frame_fn)(const solar_os_rtp_jpeg_frame_t *frame, void *user);

typedef struct {
    uint32_t frames, dropped_frames;
    bool collecting;
} solar_os_native_rtp_jpeg_stats_v1_t;

/* Header payload and parsed JPEG views borrow the input bytes. Packet and
 * frame visitors run synchronously and borrow data only for the callback.
 * A nonzero result from a visitor stops the operation and is returned to
 * the caller. Sender counters advance for each packet offered to its visitor.
 * JPEG receivers use a caller-owned buffer through close. Keep that buffer
 * independent of incoming packets. While a receiver's operation or visitor
 * is active, subsequent operations on the same handle return BUSY. */
typedef struct {
    uint32_t abi_version, struct_size;
    bool (*track_valid)(const solar_os_media_track_t *track);
    int (*clock_init)(solar_os_media_clock_t *clock, uint64_t origin_us, uint32_t timestamp, uint32_t rate);
    int (*clock_map)(const solar_os_media_clock_t *clock, uint64_t timestamp_us, uint32_t *timestamp);
    int (*header_encode)(const solar_os_rtp_header_t *header, uint8_t *packet, size_t capacity);
    int (*header_decode)(const uint8_t *packet, size_t length, solar_os_rtp_header_t *header,
        const uint8_t **payload, size_t *payload_length);
    bool (*sender_valid)(const solar_os_rtp_sender_t *sender);
    int (*l16_packetize)(solar_os_rtp_sender_t *sender, const int16_t *samples, size_t frames,
        uint8_t channels, uint8_t *packet, size_t capacity, solar_os_native_rtp_packet_fn visit, void *user);
    int (*sender_report)(uint32_t ssrc, uint32_t ntp_seconds, uint32_t ntp_fraction, uint32_t timestamp,
        uint32_t packets, uint32_t octets, const char *cname, uint8_t *packet, size_t capacity, size_t *length);
    int (*bye)(uint32_t ssrc, uint8_t *packet, size_t capacity, size_t *length);
    int (*jpeg_parse)(const uint8_t *jpeg, size_t length, solar_os_rtp_jpeg_view_t *view);
    int (*jpeg_packetize)(solar_os_rtp_sender_t *sender, const solar_os_rtp_jpeg_view_t *view,
        uint8_t *packet, size_t capacity, solar_os_native_rtp_packet_fn visit, void *user);
    int (*jpeg_receiver_open)(uint8_t payload_type, uint8_t *buffer, size_t capacity, solar_os_native_handle_t *receiver);
    int (*jpeg_receiver_close)(solar_os_native_handle_t receiver);
    int (*jpeg_receiver_reset)(solar_os_native_handle_t receiver);
    int (*jpeg_receiver_feed)(solar_os_native_handle_t receiver, const uint8_t *packet, size_t length,
        solar_os_native_rtp_jpeg_frame_fn visit, void *user, bool *complete);
    int (*jpeg_receiver_status)(solar_os_native_handle_t receiver, solar_os_native_rtp_jpeg_stats_v1_t *status);
} solar_os_native_rtp_api_v1_t;

/* Operates on caller-owned buffers. A zero header length means the request
 * is incomplete. Method labels have firmware lifetime. */
typedef struct {
    uint32_t abi_version, struct_size;
    size_t (*header_length)(const uint8_t *data, size_t length);
    int (*parse_request)(const uint8_t *data, size_t length, solar_os_rtsp_request_t *request);
    const char *(*method_name)(solar_os_rtsp_method_t method);
} solar_os_native_rtsp_protocol_api_v1_t;
