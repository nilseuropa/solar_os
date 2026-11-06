#include "solar_os_rtspd_job.h"
#include "solar_os_wifi.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "solar_os_camera.h"
#include "solar_os_config.h"
#include "solar_os_stream.h"
#include "solar_os_rtspd_options.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_media.h"
#include "solar_os_memory.h"
#include "solar_os_rtp.h"
#include "solar_os_rtp_jpeg.h"
#include "solar_os_rtsp.h"
#include "solar_os_shell_io.h"
#include "solar_os_task.h"

#define RTSPD_OWNER "job:rtspd"
#define RTSPD_TASK_STACK 8192U
#define RTSPD_SOURCE_STACK 4096U
#define RTSPD_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define RTSPD_STOP_WAIT_MS 5000U
#define RTSPD_SELECT_MS 100U
#define RTSPD_RTSP_BUFFER_BYTES 1024U
#define RTSPD_RTCP_INTERVAL_US 5000000LL
#define RTSPD_UDP_PORT_FIRST 50000U
#define RTSPD_UDP_PORT_LAST 50198U
#define RTSPD_VIDEO 0U
#define RTSPD_AUDIO 1U
#define RTSPD_TRACKS 2U

typedef struct {
    int rtp_fd;
    int rtcp_fd;
    struct sockaddr_in rtp_target;
    struct sockaddr_in rtcp_target;
    uint16_t server_rtp_port;
    uint16_t server_rtcp_port;
    bool setup;
    solar_os_rtp_sender_t sender;
    solar_os_media_clock_t clock;
    uint32_t packet_count;
    uint32_t octet_count;
    uint64_t last_video_us;
    int64_t retry_after_us;
} rtspd_track_t;

typedef struct {
    int client_fd;
    struct sockaddr_in peer;
    char session[SOLAR_OS_RTSP_SESSION_MAX];
    char local_ip[INET_ADDRSTRLEN];
    char cname[48];
    bool playing;
    volatile bool close_requested;
    uint8_t input[RTSPD_RTSP_BUFFER_BYTES];
    size_t input_len;
    rtspd_track_t tracks[RTSPD_TRACKS];
    int64_t next_rtcp_us;
} rtspd_session_t;

typedef struct {
    TaskHandle_t task;
    void *scratch;
    uint32_t stack_min_free;
    volatile bool ready;
    volatile bool done;
    bool busy;
    esp_err_t start_error;
} rtspd_source_t;

typedef struct {
    solar_os_stream_handle_t source;
    solar_os_stream_video_frame_t frame;
    uint8_t packet[SOLAR_OS_MEDIA_RTP_PACKET_MAX];
} rtspd_video_scratch_t;

typedef struct {
    int16_t samples[(SOLAR_OS_MEDIA_RTP_PACKET_MAX - SOLAR_OS_RTP_HEADER_BYTES) / 2U];
    uint8_t packet[SOLAR_OS_MEDIA_RTP_PACKET_MAX];
} rtspd_audio_scratch_t;

typedef struct {
    bool running;
    bool wifi_latency_lease;
    volatile bool stop_requested;
    volatile bool worker_done;
    TaskHandle_t worker_task;
    int listen_fd;
    int client_fd;
    solar_os_rtspd_options_t options;
    solar_os_stream_audio_format_t audio_format;
    rtspd_source_t sources[RTSPD_TRACKS];
    rtspd_session_t *session;
    rtspd_session_t *active_session;
    uint32_t clients;
    uint32_t rejected_clients;
    uint32_t frames;
    uint32_t audio_blocks;
    uint32_t dropped_frames;
    uint32_t rtp_packets;
    uint32_t rtcp_reports;
    uint64_t rtp_octets;
    uint32_t capture_errors;
    uint32_t jpeg_errors;
    uint32_t send_errors;
    uint32_t congestion_drops;
    int last_send_errno;
    esp_err_t last_error;
} rtspd_state_t;

typedef struct {
    rtspd_session_t *session;
    rtspd_track_t *track;
} rtspd_packet_context_t;

static const char *TAG = "rtspd";
static rtspd_state_t rtspd = {.listen_fd = -1, .client_fd = -1};
static portMUX_TYPE rtspd_lock = portMUX_INITIALIZER_UNLOCKED;

static void rtspd_note_stack(unsigned index)
{
    /* ESP-IDF reports bytes, unlike upstream FreeRTOS's word count. */
    const uint32_t free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&rtspd_lock);
    rtspd.sources[index].stack_min_free = free_bytes;
    portEXIT_CRITICAL(&rtspd_lock);
}

static bool rtspd_should_stop(void)
{
    return rtspd.stop_requested;
}

static bool rtspd_track_enabled(unsigned index)
{
    return index == RTSPD_VIDEO ? rtspd.options.video : rtspd.options.audio[0] != '\0';
}

/* Readers reference immutable session transport state only while busy.
 * Closing PLAY first revokes new references, then waits before closing UDP. */
static rtspd_session_t *rtspd_source_enter(unsigned index)
{
    portENTER_CRITICAL(&rtspd_lock);
    rtspd_session_t *session = rtspd.active_session;
    if (session && session->tracks[index].setup) rtspd.sources[index].busy = true;
    else session = NULL;
    portEXIT_CRITICAL(&rtspd_lock);
    return session;
}

static void rtspd_source_leave(unsigned index)
{
    portENTER_CRITICAL(&rtspd_lock);
    rtspd.sources[index].busy = false;
    portEXIT_CRITICAL(&rtspd_lock);
}

static bool rtspd_end_play(void)
{
    portENTER_CRITICAL(&rtspd_lock);
    rtspd.active_session = NULL;
    portEXIT_CRITICAL(&rtspd_lock);
    const int64_t deadline = esp_timer_get_time() + 1000000LL;
    for (;;) {
        portENTER_CRITICAL(&rtspd_lock);
        const bool busy = rtspd.sources[0].busy || rtspd.sources[1].busy;
        portEXIT_CRITICAL(&rtspd_lock);
        if (!busy) return true;
        if (esp_timer_get_time() >= deadline) {
            rtspd.stop_requested = true;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}

static void rtspd_close_fd(int *fd)
{
    if (*fd >= 0) {
        (void)shutdown(*fd, SHUT_RDWR);
        (void)close(*fd);
        *fd = -1;
    }
}

static esp_err_t rtspd_send_all(int fd, const char *data, size_t length)
{
    size_t offset = 0U;
    while (offset < length && !rtspd_should_stop()) {
        const ssize_t written = send(fd, data + offset, length - offset, 0);
        if (written > 0) {
            offset += (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return ESP_FAIL;
        }
    }
    return offset == length ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t rtspd_send_response(int fd,
                                      uint32_t cseq,
                                      int status,
                                      const char *reason,
                                      const char *headers,
                                      const char *body)
{
    const size_t body_len = body != NULL ? strlen(body) : 0U;
    char response[1200];
    int written = 0;
    if (body != NULL) {
        written = snprintf(
            response,
            sizeof(response),
            "RTSP/1.0 %d %s\r\nCSeq: %" PRIu32
            "\r\nServer: SolarOS-Media/1.0\r\n%s"
            "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s",
            status,
            reason,
            cseq,
            headers != NULL ? headers : "",
            (unsigned)body_len,
            body);
    } else {
        written = snprintf(
            response,
            sizeof(response),
            "RTSP/1.0 %d %s\r\nCSeq: %" PRIu32
            "\r\nServer: SolarOS-Media/1.0\r\n%s\r\n",
            status,
            reason,
            cseq,
            headers != NULL ? headers : "");
    }
    if (written < 0 || (size_t)written >= sizeof(response)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return rtspd_send_all(fd, response, (size_t)written);
}

static esp_err_t rtspd_open_listener(uint16_t port, int *listen_fd)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return ESP_FAIL;
    }
    const int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 1) != 0) {
        rtspd_close_fd(&fd);
        return ESP_FAIL;
    }
    *listen_fd = fd;
    return ESP_OK;
}

static esp_err_t rtspd_open_udp_pair(rtspd_track_t *track)
{
    for (uint16_t port = RTSPD_UDP_PORT_FIRST; port <= RTSPD_UDP_PORT_LAST;
         port = (uint16_t)(port + 2U)) {
        int rtp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        int rtcp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (rtp_fd < 0 || rtcp_fd < 0) {
            rtspd_close_fd(&rtp_fd);
            rtspd_close_fd(&rtcp_fd);
            return ESP_FAIL;
        }
        struct sockaddr_in address = {
            .sin_family = AF_INET, .sin_port = htons(port),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        const int rtp_bound = bind(rtp_fd, (const struct sockaddr *)&address,
                                    sizeof(address));
        address.sin_port = htons((uint16_t)(port + 1U));
        if (rtp_bound == 0 &&
            bind(rtcp_fd, (const struct sockaddr *)&address, sizeof(address)) == 0 &&
            fcntl(rtp_fd, F_SETFL, O_NONBLOCK) == 0 &&
            fcntl(rtcp_fd, F_SETFL, O_NONBLOCK) == 0) {
            track->rtp_fd = rtp_fd;
            track->rtcp_fd = rtcp_fd;
            track->server_rtp_port = port;
            track->server_rtcp_port = (uint16_t)(port + 1U);
            return ESP_OK;
        }
        rtspd_close_fd(&rtp_fd);
        rtspd_close_fd(&rtcp_fd);
    }
    return ESP_ERR_NOT_FOUND;
}

static void rtspd_close_udp(rtspd_track_t *track)
{
    rtspd_close_fd(&track->rtp_fd);
    rtspd_close_fd(&track->rtcp_fd);
    track->setup = false;
}

static bool rtspd_session_matches(const rtspd_session_t *session,
                                   const solar_os_rtsp_request_t *request)
{
    return request->session[0] == '\0' ||
        strcmp(request->session, session->session) == 0;
}

static esp_err_t rtspd_describe(rtspd_session_t *session,
                                 const solar_os_rtsp_request_t *request)
{
    char sdp[768];
    int used = snprintf(sdp, sizeof(sdp),
        "v=0\r\n"
        "o=- %s 1 IN IP4 %s\r\n"
        "s=SolarOS Media\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n",
        session->session, session->local_ip, session->local_ip);
    if (used < 0 || (size_t)used >= sizeof(sdp)) return ESP_ERR_INVALID_SIZE;
    if (rtspd.options.video) {
        const int n = snprintf(sdp + used, sizeof(sdp) - (size_t)used,
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 JPEG/90000\r\n"
            "a=control:trackID=0\r\n");
        if (n < 0 || (size_t)n >= sizeof(sdp) - (size_t)used)
            return ESP_ERR_INVALID_SIZE;
        used += n;
        if (rtspd.options.fps) {
            const int n = snprintf(sdp + used, sizeof(sdp) - (size_t)used,
                                      "a=framerate:%u\r\n", rtspd.options.fps);
            if (n < 0 || (size_t)n >= sizeof(sdp) - (size_t)used)
                return ESP_ERR_INVALID_SIZE;
            used += n;
        }
    }
    if (rtspd.options.audio[0]) {
        const int n = snprintf(sdp + used, sizeof(sdp) - (size_t)used,
            "m=audio 0 RTP/AVP 97\r\n"
            "a=rtpmap:97 L16/%" PRIu32 "/%u\r\n"
            "a=control:trackID=1\r\n",
            rtspd.audio_format.sample_rate, rtspd.audio_format.channels);
        if (n < 0 || (size_t)n >= sizeof(sdp) - (size_t)used)
            return ESP_ERR_INVALID_SIZE;
    }
    char headers[256];
    const int n = snprintf(headers, sizeof(headers),
        "Content-Base: rtsp://%s:%u/media/\r\n",
        session->local_ip, rtspd.options.port);
    if (n < 0 || (size_t)n >= sizeof(headers)) return ESP_ERR_INVALID_SIZE;
    return rtspd_send_response(session->client_fd, request->cseq, 200, "OK",
                                 headers, sdp);
}

static esp_err_t rtspd_setup(rtspd_session_t *session,
                              const solar_os_rtsp_request_t *request)
{
    const char *suffix = strrchr(request->uri, '/');
    const unsigned index = suffix && !strcmp(suffix, "/trackID=0") ? RTSPD_VIDEO :
        suffix && !strcmp(suffix, "/trackID=1") ? RTSPD_AUDIO : RTSPD_TRACKS;
    if (index >= RTSPD_TRACKS || !rtspd_track_enabled(index) ||
        !request->client_rtp_port || !rtspd_session_matches(session, request)) {
        return rtspd_send_response(session->client_fd, request->cseq, 454,
                                     "Session Not Found", NULL, NULL);
    }
    if (session->playing) {
        return rtspd_send_response(session->client_fd, request->cseq, 455,
                                     "Method Not Valid in This State", NULL, NULL);
    }
    rtspd_track_t *track = &session->tracks[index];
    rtspd_close_udp(track);
    if (rtspd_open_udp_pair(track) != ESP_OK) {
        return rtspd_send_response(session->client_fd, request->cseq, 500,
                                     "Internal Server Error", NULL, NULL);
    }
    track->rtp_target = session->peer;
    track->rtp_target.sin_port = htons(request->client_rtp_port);
    track->rtcp_target = session->peer;
    track->rtcp_target.sin_port = htons(request->client_rtcp_port);
    track->setup = true;
    char headers[320];
    const int n = snprintf(headers, sizeof(headers),
        "Session: %s;timeout=60\r\n"
        "Transport: RTP/AVP/UDP;unicast;client_port=%u-%u;server_port=%u-%u;ssrc=%08" PRIX32 "\r\n",
        session->session, request->client_rtp_port, request->client_rtcp_port,
        track->server_rtp_port, track->server_rtcp_port, track->sender.ssrc);
    if (n < 0 || (size_t)n >= sizeof(headers)) return ESP_ERR_INVALID_SIZE;
    return rtspd_send_response(session->client_fd, request->cseq, 200, "OK",
                                 headers, NULL);
}

static esp_err_t rtspd_play(rtspd_session_t *session,
                             const solar_os_rtsp_request_t *request)
{
    if ((!session->tracks[0].setup && !session->tracks[1].setup) ||
        !rtspd_session_matches(session, request)) {
        return rtspd_send_response(session->client_fd, request->cseq, 454,
                                     "Session Not Found", NULL, NULL);
    }
    if (session->playing) {
        return rtspd_send_response(session->client_fd, request->cseq, 455,
                                     "Method Not Valid in This State", NULL, NULL);
    }
    char headers[640];
    int used = snprintf(headers, sizeof(headers),
                           "Session: %s;timeout=60\r\nRTP-Info: ", session->session);
    if (used < 0 || (size_t)used >= sizeof(headers)) return ESP_ERR_INVALID_SIZE;
    bool first = true;
    for (unsigned i = 0; i < RTSPD_TRACKS; i++) {
        rtspd_track_t *track = &session->tracks[i];
        if (!track->setup) continue;
        const int n = snprintf(headers + used, sizeof(headers) - (size_t)used,
            "%surl=rtsp://%s:%u/media/trackID=%u;seq=%u;rtptime=%" PRIu32,
            first ? "" : ",", session->local_ip, rtspd.options.port, i,
            track->sender.sequence, track->sender.timestamp);
        if (n < 0 || (size_t)n + 2U >= sizeof(headers) - (size_t)used)
            return ESP_ERR_INVALID_SIZE;
        used += n;
        first = false;
    }
    strcpy(headers + used, "\r\n");
    const esp_err_t error = rtspd_send_response(
        session->client_fd, request->cseq, 200, "OK", headers, NULL);
    if (error == ESP_OK) {
        const uint64_t now_us = (uint64_t)esp_timer_get_time();
        for (unsigned i = 0; i < RTSPD_TRACKS; i++) {
            rtspd_track_t *track = &session->tracks[i];
            if (!track->setup) continue;
            (void)solar_os_media_clock_init(&track->clock, now_us,
                track->sender.timestamp,
                i == RTSPD_VIDEO ? 90000U : rtspd.audio_format.sample_rate);
        }
        session->playing = true;
        session->next_rtcp_us = (int64_t)now_us + RTSPD_RTCP_INTERVAL_US;
        portENTER_CRITICAL(&rtspd_lock);
        rtspd.active_session = session;
        portEXIT_CRITICAL(&rtspd_lock);
    }
    return error;
}

static esp_err_t rtspd_handle_request(
    rtspd_session_t *session,
    const solar_os_rtsp_request_t *request)
{
    switch (request->method) {
    case SOLAR_OS_RTSP_METHOD_OPTIONS:
        return rtspd_send_response(
            session->client_fd,
            request->cseq,
            200,
            "OK",
            "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n",
            NULL);
    case SOLAR_OS_RTSP_METHOD_DESCRIBE:
        return rtspd_describe(session, request);
    case SOLAR_OS_RTSP_METHOD_SETUP:
        return rtspd_setup(session, request);
    case SOLAR_OS_RTSP_METHOD_PLAY:
        return rtspd_play(session, request);
    case SOLAR_OS_RTSP_METHOD_GET_PARAMETER: {
        char header[64];
        const int written = snprintf(header,
                                     sizeof(header),
                                     "Session: %s;timeout=60\r\n",
                                     session->session);
        return written > 0 && (size_t)written < sizeof(header) ?
            rtspd_send_response(
                session->client_fd, request->cseq, 200, "OK", header, NULL) :
            ESP_ERR_INVALID_SIZE;
    }
    case SOLAR_OS_RTSP_METHOD_TEARDOWN:
        if (!rtspd_session_matches(session, request)) {
            return rtspd_send_response(session->client_fd,
                                        request->cseq,
                                        454,
                                        "Session Not Found",
                                        NULL,
                                        NULL);
        }
        session->close_requested = true;
        return rtspd_send_response(
            session->client_fd, request->cseq, 200, "OK", NULL, NULL);
    case SOLAR_OS_RTSP_METHOD_UNSUPPORTED:
    default:
        return rtspd_send_response(session->client_fd,
                                    request->cseq,
                                    405,
                                    "Method Not Allowed",
                                    "Allow: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n",
                                    NULL);
    }
}

static esp_err_t rtspd_read_requests(rtspd_session_t *session)
{
    if (session->input_len >= sizeof(session->input)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const ssize_t received = recv(session->client_fd,
                                  &session->input[session->input_len],
                                  sizeof(session->input) - session->input_len,
                                  0);
    if (received <= 0) {
        return ESP_FAIL;
    }
    session->input_len += (size_t)received;
    for (;;) {
        const size_t header_len = solar_os_rtsp_header_length(
            session->input, session->input_len);
        if (header_len == 0U) {
            return session->input_len < sizeof(session->input) ?
                ESP_OK : ESP_ERR_INVALID_SIZE;
        }
        solar_os_rtsp_request_t request;
        esp_err_t error = solar_os_rtsp_parse_request(
            session->input, header_len, &request);
        if (error == ESP_ERR_NOT_SUPPORTED) {
            error = rtspd_send_response(session->client_fd,
                                         request.cseq,
                                         461,
                                         "Unsupported Transport",
                                         NULL,
                                         NULL);
        } else if (error != ESP_OK) {
            error = rtspd_send_response(session->client_fd,
                                         request.cseq,
                                         400,
                                         "Bad Request",
                                         NULL,
                                         NULL);
        } else {
            error = rtspd_handle_request(session, &request);
        }
        const size_t remaining = session->input_len - header_len;
        memmove(session->input, &session->input[header_len], remaining);
        session->input_len = remaining;
        if (error != ESP_OK || session->close_requested || remaining == 0U) {
            return error;
        }
    }
}

static esp_err_t rtspd_send_rtp_packet(const uint8_t *packet,
                                       size_t packet_len, void *user)
{
    rtspd_packet_context_t *context = user;
    rtspd_session_t *session = context->session;
    rtspd_track_t *track = context->track;
    portENTER_CRITICAL(&rtspd_lock);
    const bool active = rtspd.active_session == session && !rtspd.stop_requested;
    portEXIT_CRITICAL(&rtspd_lock);
    if (!active) return ESP_ERR_INVALID_STATE;
    const int64_t now_us = esp_timer_get_time();
    if (now_us < track->retry_after_us) return ESP_ERR_TIMEOUT;
    const ssize_t sent = sendto(track->rtp_fd, packet, packet_len, 0,
               (const struct sockaddr *)&track->rtp_target,
               sizeof(track->rtp_target));
    if (sent != (ssize_t)packet_len) {
        const int send_errno = sent < 0 ? errno : EIO;
        portENTER_CRITICAL(&rtspd_lock);
        rtspd.last_send_errno = send_errno;
        portEXIT_CRITICAL(&rtspd_lock);
        if (send_errno == EAGAIN || send_errno == EWOULDBLOCK ||
            send_errno == ENOMEM || send_errno == ENOBUFS) {
            /* Temporary TX pressure is not an RTSP disconnect. Drop this
             * frame/block and freshly captured data during a short backoff;
             * never queue it or retain the camera framebuffer to retry. */
            track->retry_after_us = now_us + 50000LL;
            return ESP_ERR_TIMEOUT;
        }
        return ESP_FAIL;
    }
    portENTER_CRITICAL(&rtspd_lock);
    track->packet_count++;
    track->octet_count += (uint32_t)(packet_len - SOLAR_OS_RTP_HEADER_BYTES);
    rtspd.rtp_packets++;
    rtspd.rtp_octets += packet_len - SOLAR_OS_RTP_HEADER_BYTES;
    portEXIT_CRITICAL(&rtspd_lock);
    return ESP_OK;
}

static void rtspd_send_error(rtspd_session_t *session, esp_err_t error)
{
    /* Revoking PLAY is normal cancellation, not a transport failure. */
    if (error == ESP_OK || error == ESP_ERR_INVALID_STATE) return;
    portENTER_CRITICAL(&rtspd_lock);
    if (error == ESP_ERR_TIMEOUT) rtspd.congestion_drops++;
    else rtspd.send_errors++;
    rtspd.last_error = error;
    if (error != ESP_ERR_TIMEOUT) session->close_requested = true;
    portEXIT_CRITICAL(&rtspd_lock);
}

static void rtspd_video_reader(void *arg)
{
    (void)arg;
    rtspd_video_scratch_t *scratch = rtspd.sources[RTSPD_VIDEO].scratch;
    uint8_t *packet = scratch->packet;
    const bool vga = rtspd.options.camera.frame_size == SOLAR_OS_CAMERA_FRAME_SIZE_VGA;
    const solar_os_stream_open_options_t options = {
        .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
        .requested_video = {.codec = SOLAR_OS_STREAM_VIDEO_JPEG,
            .width = vga ? 640U : 320U, .height = vga ? 480U : 240U,
            .jpeg_quality = rtspd.options.camera.jpeg_quality},
    };
    esp_err_t error = solar_os_stream_open_ex(rtspd.options.video_source,
                                              RTSPD_OWNER, &options, &scratch->source);
    rtspd_note_stack(RTSPD_VIDEO);
    portENTER_CRITICAL(&rtspd_lock);
    rtspd.sources[RTSPD_VIDEO].start_error = error;
    rtspd.sources[RTSPD_VIDEO].ready = true;
    portEXIT_CRITICAL(&rtspd_lock);
    while (error == ESP_OK && !rtspd_should_stop()) {
        solar_os_stream_video_frame_t *frame = &scratch->frame;
        error = solar_os_stream_acquire_frame(&scratch->source, frame);
        rtspd_note_stack(RTSPD_VIDEO);
        if (error != ESP_OK) {
            portENTER_CRITICAL(&rtspd_lock);
            rtspd.capture_errors++;
            rtspd.last_error = error;
            portEXIT_CRITICAL(&rtspd_lock);
            vTaskDelay(pdMS_TO_TICKS(1U));
            error = ESP_OK;
            continue;
        }
        rtspd_session_t *session = rtspd_source_enter(RTSPD_VIDEO);
        if (session) {
            rtspd_track_t *track = &session->tracks[RTSPD_VIDEO];
            if (solar_os_rtspd_video_due(frame->timestamp_us, track->clock.origin_us,
                                          track->last_video_us, rtspd.options.fps)) {
                solar_os_rtp_jpeg_view_t jpeg;
                error = solar_os_rtp_jpeg_parse(frame->data, frame->length, &jpeg);
                if (error == ESP_OK) {
                    error = solar_os_media_clock_map(&track->clock, frame->timestamp_us,
                                                      &track->sender.timestamp);
                }
                rtspd_packet_context_t context = {.session = session, .track = track};
                if (error == ESP_OK) {
                    error = solar_os_rtp_jpeg_packetize(&track->sender, &jpeg, packet,
                        SOLAR_OS_MEDIA_RTP_PACKET_MAX, rtspd_send_rtp_packet, &context);
                }
                portENTER_CRITICAL(&rtspd_lock);
                if (error == ESP_OK) {
                    rtspd.frames++;
                    track->last_video_us = frame->timestamp_us;
                } else if (error == ESP_ERR_NOT_SUPPORTED ||
                           error == ESP_ERR_INVALID_RESPONSE) {
                    rtspd.jpeg_errors++;
                    rtspd.last_error = error;
                } else if (error == ESP_ERR_TIMEOUT) {
                    rtspd.dropped_frames++;
                }
                portEXIT_CRITICAL(&rtspd_lock);
                if (error != ESP_ERR_NOT_SUPPORTED && error != ESP_ERR_INVALID_RESPONSE)
                    rtspd_send_error(session, error);
            } else {
                portENTER_CRITICAL(&rtspd_lock);
                rtspd.dropped_frames++;
                portEXIT_CRITICAL(&rtspd_lock);
            }
        }
        error = solar_os_stream_release_frame(&scratch->source, frame);
        if (session) {
            rtspd_send_error(session, error);
            rtspd_source_leave(RTSPD_VIDEO);
        }
        rtspd_note_stack(RTSPD_VIDEO);
    }
    if (scratch->source.slot >= 0 && scratch->source.leased_frame == NULL) {
        const esp_err_t close_error = solar_os_stream_close_ex(&scratch->source);
        if (error == ESP_OK) error = close_error;
    }
    if (error != ESP_OK) {
        portENTER_CRITICAL(&rtspd_lock);
        rtspd.last_error = error;
        rtspd.stop_requested = true;
        portEXIT_CRITICAL(&rtspd_lock);
    }
    rtspd.sources[RTSPD_VIDEO].done = true;
    solar_os_task_delete_internal(NULL);
}

static bool rtspd_audio_format_valid(const solar_os_stream_audio_format_t *format)
{
    return format->sample_format == SOLAR_OS_STREAM_AUDIO_S16_LE &&
        format->bits_per_sample == 16U && format->channels >= 1U &&
        format->channels <= 8U && format->sample_rate >= 8000U &&
        format->sample_rate <= 192000U;
}

static void rtspd_audio_reader(void *arg)
{
    (void)arg;
    rtspd_audio_scratch_t *scratch = rtspd.sources[RTSPD_AUDIO].scratch;
    solar_os_stream_handle_t source = SOLAR_OS_STREAM_HANDLE_INIT;
    const solar_os_stream_open_options_t options = {
        .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
        .timeout_ms = 100U,
        .requested_audio = rtspd.audio_format,
    };
    esp_err_t error = solar_os_stream_open_ex(rtspd.options.audio, RTSPD_OWNER,
                                               &options, &source);
    if (error == ESP_OK && !rtspd_audio_format_valid(&source.audio))
        error = ESP_ERR_NOT_SUPPORTED;
    rtspd_note_stack(RTSPD_AUDIO);
    portENTER_CRITICAL(&rtspd_lock);
    if (error == ESP_OK) rtspd.audio_format = source.audio;
    rtspd.sources[RTSPD_AUDIO].start_error = error;
    rtspd.sources[RTSPD_AUDIO].ready = true;
    portEXIT_CRITICAL(&rtspd_lock);
    if (error == ESP_OK) {
        /* At most one MTU-sized PCM block. No jitter/recording queue here.
         * Opening, draining and closing all occur on this reader task. */
        const size_t frame_bytes = source.audio.channels * sizeof(int16_t);
        size_t frames = source.audio.sample_rate / 50U; /* <=20ms, MTU limited */
        if (frames > sizeof(scratch->samples) / frame_bytes)
            frames = sizeof(scratch->samples) / frame_bytes;
        const size_t bytes = frames * frame_bytes;
        while (!rtspd_should_stop()) {
            size_t read_len = 0U;
            error = solar_os_stream_read(&source, scratch->samples, bytes, 100U, &read_len);
            rtspd_note_stack(RTSPD_AUDIO);
            if (error == ESP_ERR_TIMEOUT || (error == ESP_OK && !read_len)) {
                vTaskDelay(pdMS_TO_TICKS(1U));
                continue;
            }
            if (error != ESP_OK || read_len > bytes || read_len % frame_bytes) {
                portENTER_CRITICAL(&rtspd_lock);
                rtspd.capture_errors++;
                rtspd.last_error = error == ESP_OK ? ESP_ERR_INVALID_SIZE : error;
                portEXIT_CRITICAL(&rtspd_lock);
                vTaskDelay(pdMS_TO_TICKS(1U));
                continue;
            }
            /* Native audio has no capture timestamp; estimate block start from
             * read completion. Continuous draining prevents an idle backlog. */
            const size_t captured_frames = read_len / frame_bytes;
            const uint64_t end_us = (uint64_t)esp_timer_get_time();
            const uint64_t duration_us = captured_frames * 1000000ULL / source.audio.sample_rate;
            const uint64_t captured_us = end_us >= duration_us ? end_us - duration_us : 0U;
            rtspd_session_t *session = rtspd_source_enter(RTSPD_AUDIO);
            if (!session) continue;
            rtspd_track_t *track = &session->tracks[RTSPD_AUDIO];
            if (captured_us >= track->clock.origin_us) {
                /* Anchor once, then advance in sample frames, not scheduler
                 * ticks. Packet loss never compresses the audio timeline. */
                if (!track->packet_count) {
                    error = solar_os_media_clock_map(&track->clock, captured_us,
                                                      &track->sender.timestamp);
                }
                rtspd_packet_context_t context = {.session = session, .track = track};
                if (error == ESP_OK) {
                    error = solar_os_rtp_l16_packetize(&track->sender, scratch->samples,
                        captured_frames, source.audio.channels, scratch->packet,
                        sizeof(scratch->packet),
                        rtspd_send_rtp_packet, &context);
                }
                if (error == ESP_OK) {
                    portENTER_CRITICAL(&rtspd_lock);
                    rtspd.audio_blocks++;
                    portEXIT_CRITICAL(&rtspd_lock);
                }
                rtspd_send_error(session, error);
            }
            rtspd_source_leave(RTSPD_AUDIO);
            rtspd_note_stack(RTSPD_AUDIO);
        }
    }
    solar_os_stream_close(&source);
    rtspd_note_stack(RTSPD_AUDIO);
    rtspd.sources[RTSPD_AUDIO].done = true;
    solar_os_task_delete_internal(NULL);
}

static void rtspd_send_rtcp(rtspd_session_t *session, rtspd_track_t *track)
{
    uint8_t packet[128];
    size_t packet_len = 0U;
    portENTER_CRITICAL(&rtspd_lock);
    const uint32_t packet_count = track->packet_count;
    const uint32_t octet_count = track->octet_count;
    portEXIT_CRITICAL(&rtspd_lock);
    if (!track->setup || !packet_count) return;
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    struct timeval wall_time = {0};
    (void)gettimeofday(&wall_time, NULL);
    const uint32_t ntp_seconds = (uint32_t)wall_time.tv_sec + UINT32_C(2208988800);
    const uint32_t ntp_fraction = (uint32_t)(((uint64_t)wall_time.tv_usec << 32U) / 1000000U);
    uint32_t rtp_timestamp = 0U;
    (void)solar_os_media_clock_map(&track->clock, now_us, &rtp_timestamp);
    if (solar_os_rtcp_sender_report(track->sender.ssrc, ntp_seconds, ntp_fraction,
        rtp_timestamp, packet_count, octet_count, session->cname,
        packet, sizeof(packet), &packet_len) == ESP_OK &&
        sendto(track->rtcp_fd, packet, packet_len, 0,
               (const struct sockaddr *)&track->rtcp_target,
               sizeof(track->rtcp_target)) == (ssize_t)packet_len) {
        portENTER_CRITICAL(&rtspd_lock);
        rtspd.rtcp_reports++;
        portEXIT_CRITICAL(&rtspd_lock);
    }
}

static void rtspd_send_bye(rtspd_track_t *track)
{
    if (track->rtcp_fd < 0 || !track->setup) return;
    uint8_t packet[8];
    size_t packet_len = 0U;
    if (solar_os_rtcp_bye(track->sender.ssrc, packet, sizeof(packet), &packet_len) == ESP_OK)
        (void)sendto(track->rtcp_fd, packet, packet_len, 0,
                     (const struct sockaddr *)&track->rtcp_target, sizeof(track->rtcp_target));
}

static void rtspd_reject_pending_client(int listen_fd)
{
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    const int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
    if (fd >= 0) {
        static const char response[] =
            "RTSP/1.0 453 Not Enough Bandwidth\r\nCSeq: 0\r\n"
            "Server: SolarOS-Media/1.0\r\n\r\n";
        (void)send(fd, response, sizeof(response) - 1U, 0);
        (void)close(fd);
        portENTER_CRITICAL(&rtspd_lock);
        rtspd.rejected_clients++;
        portEXIT_CRITICAL(&rtspd_lock);
    }
}

static void rtspd_serve_client(int client_fd, const struct sockaddr_in *peer,
                                int listen_fd)
{
    /* Job-owned storage outlives every reader reference, including reconnects. */
    rtspd_session_t *session = rtspd.session;
    memset(session, 0, sizeof(*session));
    session->client_fd = client_fd;
    session->peer = *peer;
    for (unsigned i = 0; i < RTSPD_TRACKS; i++) {
        rtspd_track_t *track = &session->tracks[i];
        track->rtp_fd = track->rtcp_fd = -1;
        track->sender = (solar_os_rtp_sender_t) {
            .payload_type = i == RTSPD_VIDEO ? 96U : 97U,
            .sequence = (uint16_t)esp_random(), .timestamp = esp_random(),
            .ssrc = esp_random(), .max_packet_bytes = SOLAR_OS_MEDIA_RTP_PACKET_MAX,
        };
    }
    snprintf(session->session, sizeof(session->session), "%08" PRIx32, esp_random());
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    if (getsockname(client_fd, (struct sockaddr *)&local, &local_len) != 0 ||
        inet_ntop(AF_INET, &local.sin_addr, session->local_ip,
                  sizeof(session->local_ip)) == NULL)
        strcpy(session->local_ip, "0.0.0.0");
    snprintf(session->cname, sizeof(session->cname), "solaros-%.8s@%.15s",
               session->session, session->local_ip);
    portENTER_CRITICAL(&rtspd_lock);
    rtspd.client_fd = client_fd;
    rtspd.clients++;
    portEXIT_CRITICAL(&rtspd_lock);
    while (!rtspd_should_stop() && !session->close_requested) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds);
        FD_SET(listen_fd, &read_fds);
        int max_fd = client_fd > listen_fd ? client_fd : listen_fd;
        for (unsigned i = 0; i < RTSPD_TRACKS; i++) {
            const int fd = session->tracks[i].rtcp_fd;
            if (fd < 0) continue;
            FD_SET(fd, &read_fds);
            if (fd > max_fd) max_fd = fd;
        }
        /* Control/RTCP housekeeping only. Source readers publish immediately
         * after blocking capture/read completes; no video timer or frame queue. */
        struct timeval timeout = {.tv_sec = 0, .tv_usec = RTSPD_SELECT_MS * 1000U};
        const int selected = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (selected < 0 && errno != EINTR) break;
        if (selected > 0 && FD_ISSET(listen_fd, &read_fds))
            rtspd_reject_pending_client(listen_fd);
        if (selected > 0 && FD_ISSET(client_fd, &read_fds) &&
            rtspd_read_requests(session) != ESP_OK) break;
        for (unsigned i = 0; i < RTSPD_TRACKS; i++) {
            const int fd = session->tracks[i].rtcp_fd;
            if (fd >= 0 && selected > 0 && FD_ISSET(fd, &read_fds)) {
                uint8_t report[256];
                (void)recv(fd, report, sizeof(report), 0);
            }
        }
        const int64_t now_us = esp_timer_get_time();
        if (session->playing && now_us >= session->next_rtcp_us) {
            for (unsigned i = 0; i < RTSPD_TRACKS; i++)
                rtspd_send_rtcp(session, &session->tracks[i]);
            session->next_rtcp_us = now_us + RTSPD_RTCP_INTERVAL_US;
        }
    }
    if (rtspd_end_play()) {
        for (unsigned i = 0; i < RTSPD_TRACKS; i++) {
            rtspd_send_bye(&session->tracks[i]);
            rtspd_close_udp(&session->tracks[i]);
        }
    }
    session->playing = false;
    portENTER_CRITICAL(&rtspd_lock);
    rtspd.client_fd = -1;
    portEXIT_CRITICAL(&rtspd_lock);
    (void)shutdown(client_fd, SHUT_RDWR);
    (void)close(client_fd);
}

static void rtspd_worker(void *arg)
{
    (void)arg;
    while (!rtspd_should_stop()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(rtspd.listen_fd, &read_fds);
        struct timeval timeout = {.tv_sec = 0, .tv_usec = RTSPD_SELECT_MS * 1000U};
        const int selected = select(rtspd.listen_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (selected < 0) {
            if (errno == EINTR) continue;
            if (!rtspd_should_stop()) vTaskDelay(pdMS_TO_TICKS(1U));
            continue;
        }
        if (selected == 0 || !FD_ISSET(rtspd.listen_fd, &read_fds)) continue;
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        const int client_fd = accept(rtspd.listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) continue;
        struct timeval send_timeout = {.tv_sec = 1, .tv_usec = 0};
        (void)setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO,
                         &send_timeout, sizeof(send_timeout));
        rtspd_serve_client(client_fd, &peer, rtspd.listen_fd);
    }
    rtspd.worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static esp_err_t rtspd_release_video(void)
{
    rtspd_video_scratch_t *scratch = rtspd.sources[RTSPD_VIDEO].scratch;
    if (scratch == NULL || scratch->source.slot < 0) return ESP_OK;
    if (scratch->source.leased_frame != NULL) {
        const esp_err_t error = solar_os_stream_release_frame(&scratch->source, &scratch->frame);
        if (error != ESP_OK) return error;
    }
    return solar_os_stream_close_ex(&scratch->source);
}

static bool rtspd_cleanup(void)
{
    rtspd.stop_requested = true;
    portENTER_CRITICAL(&rtspd_lock);
    rtspd.active_session = NULL;
    const int client_fd = rtspd.client_fd;
    portEXIT_CRITICAL(&rtspd_lock);
    /* Wake control select without recycling any descriptor a worker still uses. */
    if (rtspd.listen_fd >= 0) (void)shutdown(rtspd.listen_fd, SHUT_RDWR);
    if (client_fd >= 0) (void)shutdown(client_fd, SHUT_RDWR);
    if (!solar_os_task_wait_done(rtspd.worker_task, &rtspd.worker_done, RTSPD_STOP_WAIT_MS))
        return false;
    rtspd.worker_task = NULL;
    for (unsigned i = 0; i < RTSPD_TRACKS; i++) {
        rtspd_source_t *source = &rtspd.sources[i];
        if (!solar_os_task_wait_done(source->task, &source->done, RTSPD_STOP_WAIT_MS))
            return false;
        source->task = NULL;
    }
    rtspd_close_fd(&rtspd.listen_fd);
    if (rtspd.session) {
        for (unsigned i = 0; i < RTSPD_TRACKS; i++)
            rtspd_close_udp(&rtspd.session->tracks[i]);
    }
    const esp_err_t error = rtspd_release_video();
    if (error != ESP_OK) {
        rtspd.last_error = error;
        return false;
    }
    /* Only reclaim storage after all users have quiesced and leases closed. */
    solar_os_memory_free(rtspd.session);
    rtspd.session = NULL;
    for (unsigned i = 0; i < RTSPD_TRACKS; i++) {
        solar_os_memory_free(rtspd.sources[i].scratch);
        rtspd.sources[i].scratch = NULL;
    }
    if (rtspd.wifi_latency_lease) {
        (void)solar_os_wifi_latency_release(RTSPD_OWNER);
        rtspd.wifi_latency_lease = false;
    }
    rtspd.running = false;
    return true;
}

static esp_err_t rtspd_start_reader(unsigned index, TaskFunction_t reader)
{
    rtspd_source_t *source = &rtspd.sources[index];
    if (solar_os_task_create_pinned_internal(reader,
        index == RTSPD_VIDEO ? "rtsp-video" : "rtsp-audio", RTSPD_SOURCE_STACK,
        NULL, RTSPD_TASK_PRIORITY, &source->task, tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) return ESP_ERR_NO_MEM;
    const int64_t deadline = esp_timer_get_time() + RTSPD_STOP_WAIT_MS * 1000LL;
    for (;;) {
        portENTER_CRITICAL(&rtspd_lock);
        const bool ready = source->ready;
        const esp_err_t error = source->start_error;
        portEXIT_CRITICAL(&rtspd_lock);
        if (ready) return error;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}

static esp_err_t rtspd_job_start(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_rtspd_options_t options;
    solar_os_shell_io_t *io = ctx ? solar_os_context_shell_io(ctx) : NULL;
    if (!solar_os_rtspd_parse_options(argc, argv, &options)) {
        if (io) solar_os_shell_io_printf(io,
            "usage: job start rtspd [video=<stream>|none] [audio=<stream>|none] "
            "[size=qvga|vga] [fps=0..30] [port=<port>]\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (rtspd.running || rtspd.worker_task || rtspd.session ||
        rtspd.sources[0].task || rtspd.sources[1].task) return ESP_ERR_INVALID_STATE;
    if (options.video) {
        solar_os_stream_info_t video_info;
        esp_err_t error = solar_os_stream_get_info(options.video_source, &video_info);
        if (error == ESP_OK && (video_info.type != SOLAR_OS_STREAM_TYPE_VIDEO ||
            video_info.direction != SOLAR_OS_STREAM_DIRECTION_SOURCE ||
            video_info.video.codec != SOLAR_OS_STREAM_VIDEO_JPEG)) error = ESP_ERR_NOT_SUPPORTED;
        if (error != ESP_OK) {
            if (io) solar_os_shell_io_printf(io, "rtspd: video=%s must be an available JPEG source: %s\n",
                                             options.video_source, esp_err_to_name(error));
            return error;
        }
    }
    solar_os_stream_info_t audio_info = {0};
    if (options.audio[0]) {
        esp_err_t error = solar_os_stream_get_info(options.audio, &audio_info);
        if (error == ESP_OK &&
            (audio_info.type != SOLAR_OS_STREAM_TYPE_AUDIO ||
             audio_info.direction == SOLAR_OS_STREAM_DIRECTION_SINK ||
             !rtspd_audio_format_valid(&audio_info.audio))) error = ESP_ERR_NOT_SUPPORTED;
        if (error != ESP_OK) {
            if (io) solar_os_shell_io_printf(io,
                "rtspd: audio=%s must be an available S16LE PCM source: %s\n",
                options.audio, esp_err_to_name(error));
            return error;
        }
    }
    memset(&rtspd, 0, sizeof(rtspd));
    rtspd.listen_fd = rtspd.client_fd = -1;
    rtspd.options = options;
    rtspd.audio_format = audio_info.audio;
    rtspd.session = solar_os_memory_calloc(1U, sizeof(*rtspd.session),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "rtspd.session");
    if (!rtspd.session) return ESP_ERR_NO_MEM;
    for (unsigned i = 0; i < RTSPD_TRACKS; i++)
        rtspd.session->tracks[i].rtp_fd = rtspd.session->tracks[i].rtcp_fd = -1;
    for (unsigned i = 0; i < RTSPD_TRACKS; i++) {
        if (!rtspd_track_enabled(i)) continue;
        const size_t size = i == RTSPD_VIDEO ? sizeof(rtspd_video_scratch_t) :
                                               sizeof(rtspd_audio_scratch_t);
        rtspd.sources[i].scratch = solar_os_memory_alloc(size,
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
            i == RTSPD_VIDEO ? "rtspd.video" : "rtspd.audio");
        if (!rtspd.sources[i].scratch) {
            (void)rtspd_cleanup();
            return ESP_ERR_NO_MEM;
        }
        if (i == RTSPD_VIDEO) {
            rtspd_video_scratch_t *scratch = rtspd.sources[i].scratch;
            scratch->source = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
            memset(&scratch->frame, 0, sizeof(scratch->frame));
        }
    }
    esp_err_t error = solar_os_wifi_latency_acquire(RTSPD_OWNER);
    if (error == ESP_OK) rtspd.wifi_latency_lease = true;
    else if (error == ESP_ERR_INVALID_STATE) error = ESP_OK;
    if (error == ESP_OK && options.video) {
        error = rtspd_start_reader(RTSPD_VIDEO, rtspd_video_reader);
    }
    if (error == ESP_OK && options.audio[0])
        error = rtspd_start_reader(RTSPD_AUDIO, rtspd_audio_reader);
    if (error == ESP_OK) error = rtspd_open_listener(options.port, &rtspd.listen_fd);
    if (error == ESP_OK) {
        rtspd.running = true;
        if (solar_os_task_create_pinned_internal(rtspd_worker, "rtspd", RTSPD_TASK_STACK,
            NULL, RTSPD_TASK_PRIORITY, &rtspd.worker_task, tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) error = ESP_ERR_NO_MEM;
    }
    if (error != ESP_OK) {
        (void)rtspd_cleanup();
        return error;
    }
    if (options.video)
        (void)solar_os_jobs_note_resource(solar_os_rtspd_job.name,
            SOLAR_OS_JOB_RESOURCE_STREAM, options.video_source, "JPEG source lease");
    if (options.audio[0])
        (void)solar_os_jobs_note_resource(solar_os_rtspd_job.name,
            SOLAR_OS_JOB_RESOURCE_STREAM, options.audio, "L16 source");
    char resource[SOLAR_OS_JOB_RESOURCE_NAME_MAX];
    snprintf(resource, sizeof(resource), "tcp:%u", options.port);
    (void)solar_os_jobs_note_resource(solar_os_rtspd_job.name,
        SOLAR_OS_JOB_RESOURCE_NET, resource, "RTSP listen");
    if (io) solar_os_shell_io_printf(io,
        "rtspd: rtsp://<device>:%u/media video=%s audio=%s\n"
        "rtspd: WARNING: unauthenticated media stream\n",
        options.port, options.video ? options.video_source : "none",
        options.audio[0] ? options.audio : "none");
    SOLAR_OS_LOGI(TAG, "started: video=%s audio=%s fps-cap=%u port=%u",
        options.video ? options.video_source : "none", options.audio[0] ? options.audio : "none",
        options.fps, options.port);
    return ESP_OK;
}

static void rtspd_job_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (!rtspd_cleanup())
        SOLAR_OS_LOGE(TAG, "stop pending: source/control worker or camera lease still active");
}

static bool rtspd_job_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;
    if (event && event->type == SOLAR_OS_EVENT_TICK &&
        rtspd.running && rtspd.worker_done) {
        uint32_t generation = 0U;
        (void)solar_os_jobs_get_generation(solar_os_rtspd_job.name, &generation);
        if (rtspd_cleanup())
            (void)solar_os_jobs_mark_stopped(solar_os_rtspd_job.name, generation, rtspd.last_error);
    }
    return false;
}

static void rtspd_job_detail(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = ctx ? solar_os_context_shell_io(ctx) : NULL;
    if (!io) return;
    portENTER_CRITICAL(&rtspd_lock);
    const bool connected = rtspd.client_fd >= 0;
    const uint32_t clients = rtspd.clients, rejected = rtspd.rejected_clients;
    const uint32_t frames = rtspd.frames, blocks = rtspd.audio_blocks;
    const uint32_t dropped = rtspd.dropped_frames, packets = rtspd.rtp_packets;
    const uint32_t reports = rtspd.rtcp_reports;
    const uint64_t octets = rtspd.rtp_octets;
    const uint32_t capture_errors = rtspd.capture_errors, jpeg_errors = rtspd.jpeg_errors;
    const uint32_t send_errors = rtspd.send_errors;
    const uint32_t congestion = rtspd.congestion_drops;
    const int send_errno = rtspd.last_send_errno;
    const esp_err_t last_error = rtspd.last_error;
    const uint32_t video_free = rtspd.sources[RTSPD_VIDEO].stack_min_free;
    const uint32_t audio_free = rtspd.sources[RTSPD_AUDIO].stack_min_free;
    portEXIT_CRITICAL(&rtspd_lock);
    solar_os_shell_io_printf(io,
        "  RTSP: port=%u client=%s sessions=%" PRIu32 " rejected=%" PRIu32 "\n"
        "  video: source=%s size=%s fps-cap=%u frames=%" PRIu32 " dropped=%" PRIu32 "\n",
        rtspd.options.port, connected ? "connected" : "none", clients, rejected,
        rtspd.options.video ? rtspd.options.video_source : "none",
        rtspd.options.camera.frame_size == SOLAR_OS_CAMERA_FRAME_SIZE_VGA ? "vga" : "qvga",
        rtspd.options.fps, frames, dropped);
    solar_os_shell_io_printf(io,
        "  audio: source=%s L16/%" PRIu32 "/%u blocks=%" PRIu32 "\n"
        "  RTP=%" PRIu32 " octets=%" PRIu64 " RTCP=%" PRIu32 "\n",
        rtspd.options.audio[0] ? rtspd.options.audio : "none",
        rtspd.audio_format.sample_rate, rtspd.audio_format.channels, blocks,
        packets, octets, reports);
    solar_os_shell_io_printf(io,
        "  source stacks: video=%u audio=%u bytes (internal)\n"
        "  errors: capture=%" PRIu32 " jpeg=%" PRIu32 " send=%" PRIu32 " last=%s\n",
        rtspd.options.video ? RTSPD_SOURCE_STACK : 0U,
        rtspd.options.audio[0] ? RTSPD_SOURCE_STACK : 0U,
        capture_errors, jpeg_errors, send_errors, esp_err_to_name(last_error));
    solar_os_shell_io_printf(io, "  TX congestion: dropped=%" PRIu32 " errno=%d\n",
                              congestion, send_errno);
    solar_os_shell_io_printf(io,
        "  source stack min free: video=%" PRIu32 " audio=%" PRIu32 " bytes\n",
        video_free, audio_free);
    solar_os_shell_io_printf(io,
        "  runtime buffers: session=%u video=%u audio=%u bytes (PSRAM preferred)\n",
        rtspd.session ? (unsigned)sizeof(*rtspd.session) : 0U,
        rtspd.sources[RTSPD_VIDEO].scratch ? (unsigned)sizeof(rtspd_video_scratch_t) : 0U,
        rtspd.sources[RTSPD_AUDIO].scratch ? (unsigned)sizeof(rtspd_audio_scratch_t) : 0U);
}

const solar_os_job_t solar_os_rtspd_job = {
    .name = "rtspd",
    .summary = "RTSP/RTP media publisher",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = rtspd_job_start, .stop = rtspd_job_stop, .event = rtspd_job_event,
    .worker_stack_bytes = RTSPD_TASK_STACK,
    .worker_stack_external = false, .detail = rtspd_job_detail,
};
