#pragma once

#include "solar_os_rtsp.h"
#include "solar_os_rtp.h"

#define SOLAR_OS_RTSP_CLIENT_SESSION_MAX 64U
#define SOLAR_OS_RTSP_RESPONSE_MAX 8192U
#define SOLAR_OS_RTSP_AUDIO_SLOTS 32U
/* Receiver limit, not the publisher's smaller packetization MTU. */
#define SOLAR_OS_RTSP_AUDIO_PAYLOAD_MAX 1460U
#define SOLAR_OS_RTSP_JITTER_US 80000U
#define SOLAR_OS_RTSP_USER_MAX 64U
#define SOLAR_OS_RTSP_PASSWORD_MAX 64U
/* Input URL may carry userinfo; the request URI stays within SOLAR_OS_RTSP_URI_MAX. */
#define SOLAR_OS_RTSP_URL_MAX 320U

typedef struct {
    char host[128];
    char user[SOLAR_OS_RTSP_USER_MAX + 1U];
    char password[SOLAR_OS_RTSP_PASSWORD_MAX + 1U];
    uint16_t port;
    bool has_userinfo;
} solar_os_rtsp_url_t;

typedef struct {
    unsigned status;
    uint32_t cseq;
    size_t header_length, body_length;
    char session[SOLAR_OS_RTSP_CLIENT_SESSION_MAX];
    char content_base[SOLAR_OS_RTSP_URI_MAX];
    char transport[256];
    char www_authenticate[384];
} solar_os_rtsp_response_t;

typedef struct {
    bool present;
    solar_os_media_track_t media;
    char uri[SOLAR_OS_RTSP_URI_MAX];
} solar_os_rtsp_remote_track_t;

typedef struct {
    solar_os_rtsp_remote_track_t video, audio;
    char aggregate[SOLAR_OS_RTSP_URI_MAX];
} solar_os_rtsp_description_t;

typedef struct {
    bool valid;
    uint32_t ssrc, timestamp;
    uint64_t ntp_us;
} solar_os_rtsp_sender_clock_t;

typedef struct {
    bool used;
    uint16_t sequence, length;
    uint32_t timestamp;
    uint8_t payload[SOLAR_OS_RTSP_AUDIO_PAYLOAD_MAX];
} solar_os_rtsp_audio_packet_t;

typedef struct {
    solar_os_rtsp_audio_packet_t slots[SOLAR_OS_RTSP_AUDIO_SLOTS];
    bool started;
    uint32_t rate, origin, next_timestamp, ssrc;
    uint8_t channels, payload_type;
    uint64_t origin_us;
    uint32_t dropped, concealed, concealed_frames, rebuffers;
} solar_os_rtsp_audio_jitter_t;

esp_err_t solar_os_rtsp_url_parse(const char *url, solar_os_rtsp_url_t *parsed);
/* Request-URI with userinfo removed. Digest uri= uses this same string. */
esp_err_t solar_os_rtsp_url_request_uri(const char *url, char *uri, size_t capacity);
/* Display form: user@host[:port]/path, never the password. */
esp_err_t solar_os_rtsp_url_redact(const char *url, char *text, size_t capacity);
/* App address shorthand: add rtsp:// when omitted, preserving port and path.
 * Validate the resulting URL; an omitted control port defaults to 554. */
esp_err_t solar_os_rtsp_url_normalize(const char *address, char *url, size_t capacity);
esp_err_t solar_os_rtsp_uri_resolve(const char *base, const char *control,
                                   char *uri, size_t capacity);
/* TIMEOUT means a partial header/body; callers retain and append bytes. */
esp_err_t solar_os_rtsp_response_parse(const uint8_t *data, size_t length,
                                      solar_os_rtsp_response_t *response);
esp_err_t solar_os_rtsp_description_parse(const uint8_t *sdp, size_t length,
                                         const char *base,
                                         solar_os_rtsp_description_t *description);
/* Validate the negotiated UDP transport and optional server port pair. */
esp_err_t solar_os_rtsp_transport_parse(const char *transport,
                                       uint16_t client_port,
                                       uint16_t *server_rtp, uint16_t *server_rtcp);
esp_err_t solar_os_rtsp_sender_clock_feed(solar_os_rtsp_sender_clock_t *clock,
                                         const uint8_t *rtcp, size_t length);
uint64_t solar_os_rtsp_sender_time(const solar_os_rtsp_sender_clock_t *clock,
                                  uint32_t timestamp, uint32_t rate);
void solar_os_rtsp_audio_jitter_init(solar_os_rtsp_audio_jitter_t *jitter,
                                     const solar_os_media_track_t *track);
esp_err_t solar_os_rtsp_audio_jitter_feed(solar_os_rtsp_audio_jitter_t *jitter,
                                         const uint8_t *rtp, size_t length,
                                         uint64_t now_us);
bool solar_os_rtsp_audio_jitter_pop(solar_os_rtsp_audio_jitter_t *jitter,
                                    uint64_t now_us,
                                    solar_os_rtsp_audio_packet_t *packet);
