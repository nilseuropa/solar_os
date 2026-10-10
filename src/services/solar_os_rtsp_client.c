#include "solar_os_rtsp_client.h"
#include "solar_os_rtsp_auth.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "solar_os_audio_player.h"
#include "solar_os_audio_pcm.h"
#include "solar_os_memory.h"
#include "solar_os_net.h"
#include "solar_os_task.h"

#define CLIENT_TIMEOUT_US 5000000ULL
#define CLIENT_AUDIO_STACK 8192U
#define CLIENT_RX_MAX 2048U
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(CLIENT_AUDIO_STACK);

typedef struct {
    int rtp, rtcp;
    uint16_t local_port, server_rtp, server_rtcp;
    solar_os_rtsp_sender_clock_t clock;
    bool ssrc_known;
    uint32_t ssrc;
} client_track_t;

typedef struct {
    solar_os_rtsp_audio_packet_t packet;
    int16_t input[SOLAR_OS_RTSP_AUDIO_PAYLOAD_MAX / 2];
    int16_t output[2048];
    int16_t pcm[2048];
    size_t filled;
    uint32_t rebuffers;
    solar_os_audio_s16_converter_t converter;
} client_audio_scratch_t;

struct solar_os_rtsp_client {
    solar_os_rtsp_client_options_t options;
    char url[SOLAR_OS_RTSP_URI_MAX], session[SOLAR_OS_RTSP_CLIENT_SESSION_MAX];
    char user[SOLAR_OS_RTSP_USER_MAX + 1U], password[SOLAR_OS_RTSP_PASSWORD_MAX + 1U];
    solar_os_rtsp_auth_challenge_t auth;
    bool auth_ready;
    solar_os_rtsp_url_t parsed;
    solar_os_rtsp_description_t description;
    solar_os_rtsp_response_t response;
    uint32_t cseq;
    struct sockaddr_in peer;
    int control;
    client_track_t video, audio;
    SemaphoreHandle_t mutex;
    uint8_t *response_bytes, *rx, *jpeg;
    solar_os_rtsp_audio_jitter_t *jitter;
    solar_os_rtp_jpeg_receiver_t receiver;
    solar_os_rtp_jpeg_frame_t frame;
    bool pending, leased;
    bool started, running;
    uint32_t skipped_frames;
    uint32_t previous_video_drops, previous_audio_drops;
    uint32_t previous_concealed, previous_concealed_frames;
    uint32_t previous_rebuffers;
    uint64_t frame_arrived_us, audio_played_us;
    uint64_t play_started_us;
    bool received_media;
    uint32_t audio_timestamp;
    uint32_t audio_frames;
    TaskHandle_t audio_task;
    volatile bool audio_done, cancel, audio_stop;
    bool retryable;
    solar_os_rtsp_client_status_t status;
};

static uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }
static void lock(solar_os_rtsp_client_t *c) { xSemaphoreTake(c->mutex, portMAX_DELAY); }
static void unlock(solar_os_rtsp_client_t *c) { xSemaphoreGive(c->mutex); }

static esp_err_t fail(solar_os_rtsp_client_t *c, esp_err_t error, const char *format, ...)
{
    if (!c || c->cancel) return error;
    lock(c);
    if (c->status.error == ESP_OK) {
        c->status.error = error;
        va_list args; va_start(args, format);
        vsnprintf(c->status.error_detail, sizeof(c->status.error_detail), format, args);
        va_end(args);
    }
    unlock(c);
    return error;
}

static esp_err_t socket_fail(solar_os_rtsp_client_t *c, const char *operation, int error)
{
    if (c) c->retryable = error != ENOMEM && error != ENOBUFS;
    return fail(c, ESP_FAIL, "%s: %s (errno %d)", operation, strerror(error), error);
}

static esp_err_t wait_socket(solar_os_rtsp_client_t *c, int fd, bool write, uint64_t deadline,
                            const char *operation)
{
    while (!c->cancel && now_us() < deadline) {
        fd_set set; FD_ZERO(&set); FD_SET(fd, &set);
        struct timeval timeout = {.tv_usec = 20000};
        int n = select(fd + 1, write ? NULL : &set, write ? &set : NULL, NULL, &timeout);
        if (n > 0) return ESP_OK;
        if (n < 0 && errno != EINTR) return socket_fail(c, operation, errno);
    }
    c->retryable = true;
    return fail(c, ESP_ERR_TIMEOUT, "%s: timed out waiting for %s", operation,
        write ? "connection/send readiness" : "server response");
}

static esp_err_t send_bytes(solar_os_rtsp_client_t *c, const char *text, size_t len, const char *method)
{
    uint64_t deadline = now_us() + CLIENT_TIMEOUT_US;
    for (size_t sent = 0; sent < len;) {
        esp_err_t err = wait_socket(c, c->control, true, deadline, method);
        if (err != ESP_OK) return err;
        int n = send(c->control, text + sent, len - sent, 0);
        if (n > 0) sent += n;
        else if (n == 0) { c->retryable = true; return fail(c, ESP_FAIL, "%s: server closed connection during send", method); }
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return socket_fail(c, method, errno);
    }
    return ESP_OK;
}

static void auth_request_uri(const solar_os_rtsp_client_t *c, const char *original, int form,
                             char *uri, size_t capacity)
{
    const char *path = strstr(original, "://") ? strchr(original + 7, '/') : strchr(original, '/');
    if (!path || !*path) path = "/";
    /* Cameras disagree on the Digest uri. Try the public request-URI, then an
     * explicit port, then the path alone. The request line uses the same form. */
    if (form == 1)
        snprintf(uri, capacity, "rtsp://%s:%u%s", c->parsed.host,
                 c->parsed.port ? c->parsed.port : 554, path);
    else if (form == 2)
        snprintf(uri, capacity, "%s", path);
    else
        snprintf(uri, capacity, "%s", original);
}

static esp_err_t request(solar_os_rtsp_client_t *c, const char *method, const char *uri, const char *headers)
{
    int auth_tries = 0;
    int uri_form = 0;
    char uri_buf[SOLAR_OS_RTSP_URI_MAX];
    snprintf(uri_buf, sizeof(uri_buf), "%s", uri);
    for (;;) {
        char authorization[SOLAR_OS_RTSP_AUTH_HEADER_MAX];
        authorization[0] = '\0';
        if (c->auth_ready) {
            c->auth.nc++;
            esp_err_t err = solar_os_rtsp_authorization(&c->auth, method, uri_buf, c->user, c->password,
                                                      authorization, sizeof(authorization));
            if (err != ESP_OK)
                return fail(c, err, "%s: cannot build RTSP Authorization header", method);
        }
        int n = snprintf((char *)c->response_bytes, SOLAR_OS_RTSP_RESPONSE_MAX,
            "%s %s RTSP/1.0\r\nCSeq: %lu\r\nUser-Agent: SolarOS-RTSP\r\n%s%s%s%s%s%s%s\r\n",
            method, uri_buf, (unsigned long)++c->cseq,
            c->session[0] ? "Session: " : "", c->session,
            c->session[0] ? "\r\n" : "",
            authorization[0] ? "Authorization: " : "", authorization,
            authorization[0] ? "\r\n" : "", headers ? headers : "");
        if (n < 0 || (size_t)n >= SOLAR_OS_RTSP_RESPONSE_MAX)
            return fail(c, ESP_ERR_INVALID_SIZE, "%s: request exceeds control buffer", method);
        esp_err_t err = send_bytes(c, (const char *)c->response_bytes, n, method);
        if (err != ESP_OK) return err;
        size_t used = 0; uint64_t deadline = now_us() + CLIENT_TIMEOUT_US;
        for (;;) {
            err = wait_socket(c, c->control, false, deadline, method);
            if (err != ESP_OK) return err;
            n = recv(c->control, c->response_bytes + used, SOLAR_OS_RTSP_RESPONSE_MAX - used, 0);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
                if (!n) c->retryable = true;
                return n == 0 ? fail(c, ESP_FAIL, "%s: server closed connection before response", method) :
                    socket_fail(c, method, errno);
            }
            used += n;
            err = solar_os_rtsp_response_parse(c->response_bytes, used, &c->response);
            if (err != ESP_ERR_TIMEOUT) break;
            if (used == SOLAR_OS_RTSP_RESPONSE_MAX)
                return fail(c, ESP_ERR_INVALID_SIZE, "%s: response exceeds control buffer", method);
        }
        if (err != ESP_OK) return fail(c, err, "%s: malformed RTSP response (%s)", method, esp_err_to_name(err));
        if (c->response.cseq != c->cseq || used != c->response.header_length + c->response.body_length)
            return fail(c, ESP_ERR_INVALID_RESPONSE, "%s: response CSeq or body length mismatch", method);
        if (c->response.status == 401 && auth_tries < 2 && c->user[0]) {
            err = solar_os_rtsp_auth_challenge(c->response.www_authenticate, &c->auth);
            if (err != ESP_OK)
                return fail(c, err, "%s: unsupported RTSP authentication challenge", method);
            bool stale = c->auth.stale;
            if (!c->auth.cnonce[0]) {
                uint32_t nonce = esp_random();
                snprintf(c->auth.cnonce, sizeof(c->auth.cnonce), "%08lx", (unsigned long)nonce);
            }
            c->auth.nc = 0;
            c->auth_ready = true;
            auth_tries++;
            if (auth_tries == 1 || stale) continue;
            if (uri_form < 2) {
                auth_request_uri(c, uri, ++uri_form, uri_buf, sizeof(uri_buf));
                c->auth.nc = 0;
                auth_tries = 1;
                continue;
            }
        }
        if (c->response.status != 200) {
            const unsigned status = c->response.status;
            c->retryable = status == 404 || status == 453 || status == 454 || status >= 500;
            const char *reason = status == 401 ? (c->user[0] ? "authentication failed" :
                "authentication required; use rtsp://user:pass@host or rtsp-auth set") :
                status == 403 ? "access denied" : status == 404 ? "stream/path not found" :
                status == 453 ? "server receiver capacity exhausted" : status == 454 ? "session not found" :
                status == 461 ? "server rejected UDP transport" : "server rejected request";
            return fail(c, status == 401 || status == 461 ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_RESPONSE,
                "%s: RTSP %u - %s", method, status, reason);
        }
        return ESP_OK;
    }
}

static void close_track(client_track_t *t)
{
    if (t->rtp >= 0) close(t->rtp);
    if (t->rtcp >= 0) close(t->rtcp);
    t->rtp = t->rtcp = -1;
}

static esp_err_t open_udp(solar_os_rtsp_client_t *c, client_track_t *t)
{
    int last_error = EADDRINUSE;
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        uint16_t port = 10000 + (esp_random() % 24000) * 2;
        t->rtp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        t->rtcp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (t->rtp < 0 || t->rtcp < 0) {
            int error = errno; close_track(t); return socket_fail(c, "create RTP/RTCP sockets", error);
        }
        struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY), .sin_port = htons(port)};
        int bound = bind(t->rtp, (struct sockaddr *)&addr, sizeof(addr));
        addr.sin_port = htons(port + 1);
        if (!bound && !bind(t->rtcp, (struct sockaddr *)&addr, sizeof(addr))) {
            t->local_port = port;
            fcntl(t->rtp, F_SETFL, O_NONBLOCK); fcntl(t->rtcp, F_SETFL, O_NONBLOCK);
            return ESP_OK;
        }
        last_error = errno;
        close_track(t);
        if (last_error != EADDRINUSE) break;
    }
    return socket_fail(c, "bind RTP/RTCP port pair", last_error);
}

static esp_err_t setup(solar_os_rtsp_client_t *c, client_track_t *t, const char *uri)
{
    esp_err_t err = open_udp(c, t);
    if (err != ESP_OK) return err;
    char headers[128];
    snprintf(headers, sizeof(headers), "Transport: RTP/AVP/UDP;unicast;client_port=%u-%u\r\n", t->local_port, t->local_port + 1);
    err = request(c, "SETUP", uri, headers);
    if (err != ESP_OK) return err;
    if (!c->response.session[0] || (c->session[0] && strcmp(c->session, c->response.session)))
        return fail(c, ESP_ERR_INVALID_RESPONSE, "SETUP: missing or inconsistent session identifier");
    memcpy(c->session, c->response.session, sizeof(c->session));
    err = solar_os_rtsp_transport_parse(c->response.transport, t->local_port, &t->server_rtp, &t->server_rtcp);
    return err == ESP_OK ? err : fail(c, err, "SETUP: invalid/unsupported UDP Transport response");
}

static void audio_samples(const int16_t *samples, size_t count, uint8_t channels, void *user)
{
    solar_os_rtsp_client_t *c = user;
    if (c->options.samples) c->options.samples(samples, count, channels, c->options.user);
}

static bool audio_cancel(void *user)
{
    solar_os_rtsp_client_t *c = user;
    return c->cancel || c->audio_stop;
}

static void audio_worker(void *arg)
{
    solar_os_rtsp_client_t *c = arg;
    client_audio_scratch_t *s = solar_os_memory_calloc(1, sizeof(*s), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "rtsp.audio.scratch");
    solar_os_audio_player_t *player = NULL;
    solar_os_stream_audio_format_t output;
    const solar_os_media_audio_format_t *f = &c->description.audio.media.format.audio;
    const solar_os_stream_audio_format_t input = {
        .sample_rate = f->sample_rate, .channels = f->channels, .bits_per_sample = 16,
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
    };
    const solar_os_audio_player_options_t options = {
        .owner = "rtsp", .volume = SOLAR_OS_AUDIO_VOLUME_GLOBAL,
        .requested_audio = {.sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE, .bits_per_sample = 16},
        .open_timeout_ms = 500, .samples = audio_samples, .user = c,
        .should_cancel = audio_cancel, .cancel_user = c,
    };
    esp_err_t err = s ? solar_os_audio_player_create(&options, &player, &output, NULL) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) fail(c, err, s ? "default audio output unavailable (%s)" : "audio scratch allocation failed (%s)", esp_err_to_name(err));
    size_t quantum = 0;
    if (err == ESP_OK) {
        size_t frames = output.frames_per_block ? output.frames_per_block : output.sample_rate / 100U;
        quantum = frames * output.channels;
        if (!quantum || quantum > 2048)
            err = fail(c, ESP_ERR_NOT_SUPPORTED, "audio output block size %u exceeds PCM buffer", (unsigned)quantum);
        if (err == ESP_OK && c->options.diagnostics) {
            lock(c);
            c->status.audio_output_rate = output.sample_rate;
            c->status.audio_output_channels = output.channels;
            c->status.audio_block_frames = frames;
            unlock(c);
        }
    }
    while (err == ESP_OK && !audio_cancel(c)) {
        lock(c);
        bool have = solar_os_rtsp_audio_jitter_pop(c->jitter, now_us(), &s->packet);
        uint32_t rebuffers = c->jitter->rebuffers;
        c->status.audio_dropped = c->previous_audio_drops + c->jitter->dropped;
        if (c->options.diagnostics) {
            c->status.audio_concealed = c->previous_concealed + c->jitter->concealed;
            c->status.audio_concealed_frames = c->previous_concealed_frames + c->jitter->concealed_frames;
            c->status.audio_rebuffers = c->previous_rebuffers + rebuffers;
            if (!have) c->status.audio_wait_polls++;
        }
        unlock(c);
        if (s->rebuffers != rebuffers) {
            s->rebuffers = rebuffers; s->filled = 0;
            solar_os_audio_s16_converter_reset(&s->converter);
        }
        if (!have) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        size_t samples = s->packet.length / 2;
        for (size_t i = 0; i < samples; i++) s->input[i] = (int16_t)((uint16_t)s->packet.payload[i * 2] << 8 | s->packet.payload[i * 2 + 1]);
        bool done = false; uint32_t frames = 0;
        while (!done && err == ESP_OK && !audio_cancel(c)) {
            size_t count = 0;
            err = solar_os_audio_s16_convert(&s->converter, s->input, samples / f->channels, &input, &output,
                                             s->output, 2048, &count, &done);
            if (err != ESP_OK) fail(c, err, "L16 audio conversion failed (%s)", esp_err_to_name(err));
            if (err != ESP_OK || !count) continue;
            /* RTP packet boundaries are unrelated to the output's native
             * quantum. Coalesce converted PCM into complete sink blocks. */
            for (size_t consumed = 0; consumed < count && err == ESP_OK && !audio_cancel(c);) {
                size_t n = quantum - s->filled;
                if (n > count - consumed) n = count - consumed;
                memcpy(s->pcm + s->filled, s->output + consumed, n * sizeof(int16_t));
                consumed += n; s->filled += n; frames += n / output.channels;
                if (s->filled != quantum) continue;
                uint64_t before = c->options.diagnostics ? now_us() : 0;
                err = solar_os_audio_player_write(player, s->pcm, quantum * sizeof(int16_t), &c->audio_stop);
                if (err != ESP_OK) fail(c, err, "audio output write failed (%s)", esp_err_to_name(err));
                s->filled = 0;
                lock(c);
                c->audio_timestamp = s->packet.timestamp;
                c->audio_frames = (uint32_t)((uint64_t)frames * f->sample_rate / output.sample_rate);
                uint64_t submitted = now_us();
                if (c->options.diagnostics && err == ESP_OK) {
                    c->status.audio_stack_min_free = uxTaskGetStackHighWaterMark(NULL);
                    uint32_t duration = submitted - before;
                    uint32_t gap = c->audio_played_us ? submitted - c->audio_played_us : 0;
                    if (duration > c->status.audio_write_max_us) c->status.audio_write_max_us = duration;
                    if (gap > c->status.audio_gap_max_us) c->status.audio_gap_max_us = gap;
                    c->status.audio_blocks++;
                    c->status.audio_output_frames += quantum / output.channels;
                }
                c->audio_played_us = submitted; c->status.audio_playing = err == ESP_OK;
                unlock(c);
            }
        }
    }
    solar_os_audio_player_destroy(player);
    solar_os_memory_free(s);
    lock(c);
    if (err != ESP_OK && !audio_cancel(c)) { c->status.error = err; c->cancel = true; }
    c->status.audio_playing = false;
    unlock(c);
    c->audio_done = true;
    for (;;) vTaskSuspend(NULL);
}

static esp_err_t negotiate(solar_os_rtsp_client_t *c)
{
    char ip[SOLAR_OS_NET_ADDR_MAX];
    esp_err_t err = solar_os_net_resolve_host(c->parsed.host, ip, sizeof(ip));
    if (err != ESP_OK) return fail(c, err, "cannot resolve RTSP host %.64s (%s)", c->parsed.host, esp_err_to_name(err));
    c->peer.sin_family = AF_INET; c->peer.sin_port = htons(c->parsed.port);
    if (!inet_aton(ip, &c->peer.sin_addr)) return fail(c, ESP_ERR_NOT_SUPPORTED, "RTSP client requires an IPv4 address");
    c->control = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->control < 0) return socket_fail(c, "create RTSP connection", errno);
    fcntl(c->control, F_SETFL, O_NONBLOCK);
    if (connect(c->control, (struct sockaddr *)&c->peer, sizeof(c->peer)) < 0) {
        if (errno != EINPROGRESS) return socket_fail(c, "connect RTSP server", errno);
        err = wait_socket(c, c->control, true, now_us() + CLIENT_TIMEOUT_US, "connect RTSP server");
        if (err != ESP_OK) return err;
        int error = 0; socklen_t len = sizeof(error);
        if (getsockopt(c->control, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
            return socket_fail(c, "connect RTSP server", errno);
        if (error) return socket_fail(c, "connect RTSP server", error);
    }
    err = request(c, "DESCRIBE", c->url, "Accept: application/sdp\r\n");
    if (err != ESP_OK) return err;
    err = solar_os_rtsp_description_parse(c->response_bytes + c->response.header_length, c->response.body_length,
        c->response.content_base[0] ? c->response.content_base : c->url, &c->description);
    if (err != ESP_OK) return fail(c, err, "DESCRIBE: invalid SDP (%s)", esp_err_to_name(err));
    c->description.video.present &= c->options.video;
    c->description.audio.present &= c->options.audio;
    if (!c->description.video.present && !c->description.audio.present)
        return fail(c, ESP_ERR_NOT_SUPPORTED, "SDP has no supported selected tracks (JPEG video / L16 PCM audio)");
    if (c->description.video.present) {
        c->jpeg = solar_os_memory_alloc(SOLAR_OS_MEDIA_VIDEO_FRAME_MAX, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "rtsp.jpeg");
        if (!c->jpeg) return fail(c, ESP_ERR_NO_MEM, "JPEG frame buffer allocation failed (512 KiB PSRAM required)");
        err = solar_os_rtp_jpeg_receiver_init(&c->receiver, c->description.video.media.payload_type, c->jpeg, SOLAR_OS_MEDIA_VIDEO_FRAME_MAX);
        if (err == ESP_OK) err = setup(c, &c->video, c->description.video.uri);
        if (err != ESP_OK) return err;
    }
    if (c->description.audio.present) {
        c->jitter = solar_os_memory_alloc(sizeof(*c->jitter), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "rtsp.jitter");
        if (!c->jitter) return fail(c, ESP_ERR_NO_MEM, "audio jitter buffer allocation failed");
        solar_os_rtsp_audio_jitter_init(c->jitter, &c->description.audio.media);
        err = setup(c, &c->audio, c->description.audio.uri);
        if (err != ESP_OK) return err;
    }
    err = request(c, "PLAY", c->description.aggregate, NULL);
    if (err != ESP_OK) return err;
    lock(c);
    c->status.negotiated = c->status.playing = true;
    c->status.reconnecting = false;
    c->play_started_us = now_us();
    c->status.video = c->description.video.present; c->status.audio = c->description.audio.present;
    c->status.sample_rate = c->description.audio.media.clock_rate;
    c->status.channels = c->description.audio.media.format.audio.channels;
    unlock(c);
    if (c->description.audio.present) {
        c->audio_done = false;
        if (solar_os_task_create_pinned_internal(audio_worker, "rtsp-sink", CLIENT_AUDIO_STACK, c, tskIDLE_PRIORITY + 3,
            &c->audio_task, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
            c->audio_done = true; return fail(c, ESP_ERR_NO_MEM, "audio output worker could not start (internal stack/admission)");
        }
    }
    return ESP_OK;
}

static bool receive(solar_os_rtsp_client_t *c, client_track_t *t, bool rtcp)
{
    int fd = rtcp ? t->rtcp : t->rtp;
    struct sockaddr_in from; socklen_t addr_len = sizeof(from);
    int n = recvfrom(fd, c->rx, CLIENT_RX_MAX, 0, (struct sockaddr *)&from, &addr_len);
    if (n <= 0 || from.sin_addr.s_addr != c->peer.sin_addr.s_addr) return false;
    uint16_t port = rtcp ? t->server_rtcp : t->server_rtp;
    if (port && from.sin_port != htons(port)) return false;
    lock(c);
    if (rtcp) {
        solar_os_rtsp_sender_clock_t clock = t->clock;
        if (solar_os_rtsp_sender_clock_feed(&clock, c->rx, n) == ESP_OK &&
            (!t->ssrc_known || t->ssrc == clock.ssrc)) t->clock = clock;
    } else {
        solar_os_rtp_header_t h; const uint8_t *data; size_t len;
        if (solar_os_rtp_header_decode(c->rx, n, &h, &data, &len) != ESP_OK ||
            h.payload_type != (t == &c->video ? c->description.video.media.payload_type : c->description.audio.media.payload_type) ||
            (t->ssrc_known && t->ssrc != h.ssrc) || (t->clock.valid && t->clock.ssrc != h.ssrc)) {
            unlock(c); return false;
        }
        t->ssrc = h.ssrc; t->ssrc_known = true;
        c->received_media = true;
        if (t == &c->audio) {
            (void)solar_os_rtsp_audio_jitter_feed(c->jitter, c->rx, n, now_us());
        } else {
            if (c->pending && now_us() - c->frame_arrived_us > 150000) {
                c->pending = false; c->skipped_frames++;
                solar_os_rtp_jpeg_receiver_reset(&c->receiver);
            }
            /* Keep a completed JPEG stable until the decoder leases it. */
            if (c->leased || c->pending) {
                if (h.marker) c->skipped_frames++;
                c->status.video_dropped = c->previous_video_drops + c->receiver.dropped_frames + c->skipped_frames;
                unlock(c); return true;
            }
            solar_os_rtp_jpeg_frame_t frame;
            esp_err_t err = solar_os_rtp_jpeg_receiver_feed(&c->receiver, c->rx, n, &frame);
            if (err == ESP_OK && frame.data) {
                c->frame = frame; c->pending = true; c->frame_arrived_us = now_us(); c->status.video_frames++;
            }
            c->status.video_dropped = c->previous_video_drops + c->receiver.dropped_frames + c->skipped_frames;
        }
    }
    unlock(c);
    return true;
}

esp_err_t solar_os_rtsp_client_create(const char *url, const solar_os_rtsp_client_options_t *options,
                                      solar_os_rtsp_client_t **out)
{
    if (!out || !options || (!options->video && !options->audio)) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    solar_os_rtsp_url_t parsed;
    esp_err_t err = solar_os_rtsp_url_parse(url, &parsed);
    if (err != ESP_OK) return err;
    char request_uri[SOLAR_OS_RTSP_URI_MAX];
    err = solar_os_rtsp_url_request_uri(url, request_uri, sizeof(request_uri));
    if (err != ESP_OK) return err;
    if (!parsed.has_userinfo) {
        err = solar_os_rtsp_auth_lookup(parsed.host, parsed.port, parsed.user, sizeof(parsed.user),
                                       parsed.password, sizeof(parsed.password));
        if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;
        if (err != ESP_OK) return err;
    }
    solar_os_rtsp_client_t *c = solar_os_memory_calloc(1, sizeof(*c), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "rtsp.client");
    if (!c) return ESP_ERR_NO_MEM;
    c->parsed = parsed; c->options = *options; strcpy(c->url, request_uri);
    snprintf(c->user, sizeof(c->user), "%s", parsed.user);
    snprintf(c->password, sizeof(c->password), "%s", parsed.password);
    solar_os_rtsp_auth_wipe(parsed.password, sizeof(parsed.password));
    solar_os_rtsp_auth_wipe(c->parsed.password, sizeof(c->parsed.password));
    c->control = c->video.rtp = c->video.rtcp = c->audio.rtp = c->audio.rtcp = -1;
    c->audio_done = true;
    c->mutex = xSemaphoreCreateMutex();
    c->response_bytes = solar_os_memory_alloc(SOLAR_OS_RTSP_RESPONSE_MAX, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "rtsp.control");
    c->rx = solar_os_memory_alloc(CLIENT_RX_MAX, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "rtsp.packet");
    if (!c->mutex || !c->response_bytes || !c->rx) { solar_os_rtsp_client_destroy(c); return ESP_ERR_NO_MEM; }
    *out = c; return ESP_OK;
}

static esp_err_t run_session(solar_os_rtsp_client_t *c)
{
    esp_err_t err = c->cancel ? ESP_ERR_TIMEOUT : negotiate(c);
    uint64_t keepalive = now_us(), last_media = now_us();
    while (err == ESP_OK && !c->cancel) {
        fd_set set; FD_ZERO(&set); int maxfd = c->control;
        FD_SET(c->control, &set);
        int sockets[] = {c->video.rtp, c->video.rtcp, c->audio.rtp, c->audio.rtcp};
        for (size_t i = 0; i < 4; i++) if (sockets[i] >= 0) { FD_SET(sockets[i], &set); if (sockets[i] > maxfd) maxfd = sockets[i]; }
        struct timeval timeout = {.tv_usec = 10000};
        int n = select(maxfd + 1, &set, NULL, NULL, &timeout);
        if (n < 0 && errno != EINTR) { err = socket_fail(c, "receive RTSP/RTP", errno); break; }
        if (n > 0) {
            /* EOF or unsolicited RTSP traffic: do not spin indefinitely. */
            if (FD_ISSET(c->control, &set)) {
                int bytes = recv(c->control, c->rx, CLIENT_RX_MAX, 0);
                if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
                if (!bytes) c->retryable = true;
                err = bytes < 0 ? socket_fail(c, "receive RTSP control", errno) :
                    fail(c, ESP_FAIL, bytes ? "server sent unexpected RTSP control data" : "server closed RTSP connection");
                break;
            }
            for (size_t i = 0; i < 4; i++) if (sockets[i] >= 0 && FD_ISSET(sockets[i], &set)) {
                /* Bounded drain for throughput without starving cancellation. */
                for (size_t budget = 0; budget < 32 && !c->cancel; budget++)
                    if (!receive(c, i < 2 ? &c->video : &c->audio, i % 2)) break;
                    else if (!(i % 2)) last_media = now_us();
            }
        }
        if (now_us() - last_media > CLIENT_TIMEOUT_US) {
            c->retryable = true;
            err = fail(c, ESP_ERR_TIMEOUT, "no RTP media for 5 seconds (publisher stopped or UDP path blocked)"); break;
        }
        if (now_us() - keepalive > 20000000ULL) {
            err = request(c, "OPTIONS", c->description.aggregate, NULL); keepalive = now_us();
        }
    }
    /* Best-effort teardown is nonblocking even during cancellation. */
    if (c->control >= 0 && c->session[0]) {
        char authorization[SOLAR_OS_RTSP_AUTH_HEADER_MAX];
        authorization[0] = '\0';
        if (c->auth_ready)
            (void)solar_os_rtsp_authorization(&c->auth, "TEARDOWN", c->description.aggregate,
                                             c->user, c->password, authorization, sizeof(authorization));
        int len = snprintf((char *)c->response_bytes, SOLAR_OS_RTSP_RESPONSE_MAX,
            "TEARDOWN %s RTSP/1.0\r\nCSeq: %lu\r\nSession: %s\r\n%s%s%s\r\n",
            c->description.aggregate, (unsigned long)++c->cseq, c->session,
            authorization[0] ? "Authorization: " : "", authorization,
            authorization[0] ? "\r\n" : "");
        if (len > 0) (void)send(c->control, c->response_bytes, len, 0);
    }
    c->audio_stop = true;
    if (c->audio_task) {
        /* Never free a live owner or force-delete it during a stream write. */
        while (!c->audio_done) vTaskDelay(pdMS_TO_TICKS(10));
        solar_os_task_delete_internal(c->audio_task); c->audio_task = NULL;
    }
    close_track(&c->video); close_track(&c->audio);
    if (c->control >= 0) { close(c->control); c->control = -1; }
    lock(c); c->status.playing = false;
    if (c->status.error == ESP_OK) c->status.error = err;
    err = c->status.error; unlock(c);
    return err;
}

esp_err_t solar_os_rtsp_client_run(solar_os_rtsp_client_t *c)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    lock(c);
    if (c->started) { unlock(c); return ESP_ERR_INVALID_STATE; }
    c->started = c->running = true;
    c->status.epoch = 1;
    unlock(c);
    unsigned failures = 0;
    esp_err_t err;
    for (;;) {
        c->retryable = false;
        err = run_session(c);
        if (c->cancel || !c->retryable || !c->options.reconnect_attempts) break;
        /* A stable session resets the consecutive-failure budget, but a
         * server repeatedly accepting PLAY then closing cannot loop forever. */
        if (c->received_media && now_us() - c->play_started_us >= 10000000ULL) failures = 0;
        if (failures >= c->options.reconnect_attempts) break;
        unsigned delay_ms = 500U << (failures < 3 ? failures : 3);
        failures++;
        lock(c);
        c->status.reconnecting = true;
        c->status.negotiated = false;
        c->pending = false;
        unlock(c);
        uint64_t until = now_us() + delay_ms * 1000ULL;
        while (!c->cancel && now_us() < until) vTaskDelay(pdMS_TO_TICKS(10));
        /* The compressed buffer remains valid for the decoder throughout an
         * outage. Never reset/free it until its last lease is released. */
        for (;;) {
            lock(c); bool leased = c->leased; unlock(c);
            if (!leased || c->cancel) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (c->cancel) break;
        lock(c);
        c->previous_video_drops += c->receiver.dropped_frames;
        if (c->jitter) {
            c->previous_audio_drops += c->jitter->dropped;
            c->previous_concealed += c->jitter->concealed;
            c->previous_concealed_frames += c->jitter->concealed_frames;
            c->previous_rebuffers += c->jitter->rebuffers;
        }
        solar_os_memory_free(c->jpeg); c->jpeg = NULL;
        solar_os_memory_free(c->jitter); c->jitter = NULL;
        c->receiver = (solar_os_rtp_jpeg_receiver_t){0};
        c->frame = (solar_os_rtp_jpeg_frame_t){0};
        c->video.clock = c->audio.clock = (solar_os_rtsp_sender_clock_t){0};
        c->video.ssrc_known = c->audio.ssrc_known = false;
        c->description = (solar_os_rtsp_description_t){0};
        c->session[0] = 0; c->cseq = 0;
        c->auth_ready = false; c->auth.nc = 0; c->auth.stale = false;
        c->audio_played_us = c->frame_arrived_us = 0;
        c->play_started_us = 0; c->received_media = false;
        c->audio_timestamp = c->audio_frames = 0;
        c->status.error = ESP_OK; c->status.error_detail[0] = 0;
        c->status.video = c->status.audio = false;
        c->status.sample_rate = c->status.channels = 0;
        c->status.reconnects++; c->status.epoch++;
        c->audio_stop = false;
        unlock(c);
    }
    lock(c); c->running = false; c->status.reconnecting = false; unlock(c);
    return err;
}

void solar_os_rtsp_client_cancel(solar_os_rtsp_client_t *c)
{
    if (c) { c->cancel = true; c->audio_stop = true; }
}
void solar_os_rtsp_client_status(solar_os_rtsp_client_t *c, solar_os_rtsp_client_status_t *s)
{
    if (!s) return;
    if (!c) { *s = (solar_os_rtsp_client_status_t){.error = ESP_ERR_INVALID_ARG}; return; }
    lock(c); *s = c->status;
    if (c->options.diagnostics && c->jitter) {
        for (size_t i = 0; i < SOLAR_OS_RTSP_AUDIO_SLOTS; i++) s->audio_queued += c->jitter->slots[i].used;
    }
    unlock(c);
}

bool solar_os_rtsp_client_take_video(solar_os_rtsp_client_t *c, solar_os_rtp_jpeg_frame_t *frame,
                                    uint64_t *arrived_us)
{
    if (!c || !frame) return false;
    lock(c);
    bool ready = c->pending && !c->leased;
    if (ready) {
        int64_t late = (int64_t)(now_us() - c->frame_arrived_us);
        if (late > 150000) { c->pending = ready = false; c->skipped_frames++; c->status.video_dropped++; }
    }
    if (ready) {
        *frame = c->frame;
        if (arrived_us) *arrived_us = c->frame_arrived_us;
        c->pending = false; c->leased = true;
    }
    unlock(c); return ready;
}

int64_t solar_os_rtsp_client_video_lateness(solar_os_rtsp_client_t *c,
                                           uint32_t timestamp, uint64_t arrived_us)
{
    if (!c) return 0;
    lock(c);
    uint64_t now = now_us();
    int64_t late = (int64_t)(now - arrived_us) - SOLAR_OS_RTSP_JITTER_US;
    if (c->description.audio.present && c->audio.clock.valid && c->video.clock.valid &&
        c->status.audio_playing && now - c->audio_played_us < 150000) {
        uint64_t a = solar_os_rtsp_sender_time(&c->audio.clock,
            c->audio_timestamp + c->audio_frames, c->description.audio.media.clock_rate);
        uint64_t v = solar_os_rtsp_sender_time(&c->video.clock, timestamp, 90000);
        late = (int64_t)a + (int64_t)(now - c->audio_played_us) - (int64_t)v;
        /* A missing/inconsistent sender report must not freeze the viewer. */
        if (late < 0 && now - arrived_us > 150000) late = 0;
    }
    unlock(c); return late;
}

void solar_os_rtsp_client_release_video(solar_os_rtsp_client_t *c)
{
    if (!c) return;
    lock(c); c->leased = false; solar_os_rtp_jpeg_receiver_reset(&c->receiver); unlock(c);
}

esp_err_t solar_os_rtsp_client_destroy(solar_os_rtsp_client_t *c)
{
    if (!c) return ESP_OK;
    if (c->mutex) {
        lock(c);
        bool busy = c->running || c->leased;
        unlock(c);
        if (busy) return ESP_ERR_INVALID_STATE;
    }
    close_track(&c->video); close_track(&c->audio);
    if (c->control >= 0) close(c->control);
    if (c->mutex) vSemaphoreDelete(c->mutex);
    solar_os_memory_free(c->jpeg); solar_os_memory_free(c->jitter);
    solar_os_memory_free(c->response_bytes); solar_os_memory_free(c->rx);
    solar_os_rtsp_auth_wipe(c->password, sizeof(c->password));
    solar_os_rtsp_auth_wipe(c->user, sizeof(c->user));
    solar_os_memory_free(c);
    return ESP_OK;
}
