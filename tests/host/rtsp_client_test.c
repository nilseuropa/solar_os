/* Real RTSP client, response/SDP parser, RTP/JPEG assembler, L16 jitter and
 * PCM converter over loopback sockets. Only OS allocation/tasks/audio mocked. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "solar_os_rtsp_auth.h"
#include "../../src/services/solar_os_rtsp_client.c"

static atomic_uint allocations, allocation_calls, fail_allocation, live_tasks;
static atomic_uint samples_played, audio_opens, audio_closes, callbacks;
static atomic_bool fail_audio;
static bool live_probe;
static uint32_t sink_rate = 48000, sink_block = 480;
static uint8_t sink_channels = 2;
static atomic_uint random_value = 1234;

const char *esp_err_to_name(esp_err_t error)
{
    switch (error) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
    default: return "ESP_ERROR";
    }
}

void *solar_os_memory_alloc(size_t n, solar_os_memory_class_t kind, const char *tag)
{
    (void)kind; assert(!strncmp(tag, "rtsp.", 5));
    if (atomic_fetch_add(&allocation_calls, 1) + 1 == atomic_load(&fail_allocation)) return NULL;
    void *p = malloc(n); if (p) atomic_fetch_add(&allocations, 1); return p;
}
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t kind, const char *tag)
{
    void *p = solar_os_memory_alloc(n * size, kind, tag); if (p) memset(p, 0, n * size); return p;
}
void solar_os_memory_free(void *p) { if (p) { atomic_fetch_sub(&allocations, 1); free(p); } }
int64_t esp_timer_get_time(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
uint32_t esp_random(void) { return atomic_fetch_add(&random_value, 1); }
void vTaskDelay(TickType_t ticks)
{
    struct timespec t = {.tv_sec = ticks / 1000, .tv_nsec = ticks % 1000 * 1000000L}; nanosleep(&t, NULL);
}
void vTaskSuspend(TaskHandle_t task) { assert(!task); pthread_exit(NULL); }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 4096; }
struct test_task { pthread_t thread; TaskFunction_t fn; void *arg; };
static void *task_entry(void *arg) { struct test_task *t = arg; t->fn(t->arg); return NULL; }
BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t fn, const char *name, uint32_t stack,
    void *arg, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core, solar_os_task_role_t role)
{
    (void)priority; (void)core; assert(role == SOLAR_OS_TASK_ROLE_FOREGROUND && stack == 8192);
    assert(!strcmp(name, "rtsp-sink"));
    struct test_task *t = calloc(1, sizeof(*t)); assert(t);
    t->fn = fn; t->arg = arg; *handle = t; atomic_fetch_add(&live_tasks, 1);
    assert(!pthread_create(&t->thread, NULL, task_entry, t)); return pdPASS;
}
void solar_os_task_delete_internal(TaskHandle_t task)
{
    assert(task); pthread_join(task->thread, NULL); free(task); atomic_fetch_sub(&live_tasks, 1);
}
esp_err_t solar_os_net_resolve_host(const char *host, char *ip, size_t capacity)
{
    struct in_addr address;
    assert(live_probe || !strcmp(host, "127.0.0.1"));
    if (inet_pton(AF_INET, host, &address) != 1) return ESP_ERR_NOT_SUPPORTED;
    snprintf(ip, capacity, "%s", host); return ESP_OK;
}
struct solar_os_audio_player { pthread_t owner; solar_os_audio_player_options_t options; };
esp_err_t solar_os_audio_player_create(const solar_os_audio_player_options_t *o,
    solar_os_audio_player_t **p, solar_os_stream_audio_format_t *format, solar_os_audio_device_info_t *device)
{
    (void)device;
    if (atomic_load(&fail_audio)) return ESP_ERR_NOT_FOUND;
    assert(!o->buffered && o->volume == SOLAR_OS_AUDIO_VOLUME_GLOBAL && o->open_timeout_ms == 500);
    *p = malloc(sizeof(**p)); assert(*p); (*p)->owner = pthread_self(); (*p)->options = *o;
    *format = (solar_os_stream_audio_format_t){.sample_rate = sink_rate, .channels = sink_channels,
        .bits_per_sample = 16, .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE, .frames_per_block = sink_block};
    atomic_fetch_add(&audio_opens, 1); return ESP_OK;
}
esp_err_t solar_os_audio_player_write(solar_os_audio_player_t *p, const void *data,
    size_t bytes, const volatile bool *cancelled)
{
    assert(pthread_equal(p->owner, pthread_self()));
    if (*cancelled) return ESP_ERR_TIMEOUT;
    assert(bytes == sink_block * sink_channels * sizeof(int16_t)); /* Native sink blocks, not RTP fragments. */
    const int16_t *samples = data;
    if (!live_probe) assert(samples[0] == 0x1234 || samples[0] == 0); /* L16 network byte order. */
    vTaskDelay((uint32_t)(sink_block * 1000 / sink_rate));
    p->options.samples(data, bytes / 2, sink_channels, p->options.user);
    atomic_fetch_add(&samples_played, bytes / 2); return ESP_OK;
}
void solar_os_audio_player_destroy(solar_os_audio_player_t *p)
{
    if (p) { assert(pthread_equal(p->owner, pthread_self())); free(p); atomic_fetch_add(&audio_closes, 1); }
}

typedef struct {
    int listen, control;
    uint16_t port, video_port, audio_port;
    bool offer_video, offer_audio, reject, stall, relay, close_on_play, require_auth;
    unsigned reject_status;
    atomic_bool stop, playing, teardown;
    unsigned setup_video, setup_audio;
    pthread_t thread;
    client_track_t video, audio;
    struct sockaddr_in video_peer, audio_peer;
    unsigned sessions;
    atomic_uint accepted;
    atomic_bool disconnect;
} test_server_t;

static void send_response(test_server_t *s, uint32_t seq, const char *headers, const char *body)
{
    char text[2048];
    unsigned status = s->reject_status ? s->reject_status : s->reject ? 401U : 200U;
    if (headers && strstr(headers, "WWW-Authenticate:")) status = 401;
    int len = snprintf(text, sizeof(text), "RTSP/1.0 %u Test\r\nCSeq: %lu\r\n%sContent-Length: %zu\r\n\r\n%s",
        status, (unsigned long)seq, headers ? headers : "", body ? strlen(body) : 0, body ? body : "");
    assert(len > 0 && (size_t)len < sizeof(text));
    /* Deliberately fragmented headers and SDP exercise streaming framing. */
    for (int pos = 0; pos < len;) {
        int n = len - pos > 17 ? 17 : len - pos;
        int sent = send(s->control, text + pos, n, MSG_NOSIGNAL);
        if (sent <= 0) return;
        pos += sent;
    }
}

static esp_err_t send_jpeg_packet(const uint8_t *data, size_t len, void *user)
{
    test_server_t *s = user;
    assert(sendto(s->video.rtp, data, len, 0, (struct sockaddr *)&s->video_peer, sizeof(s->video_peer)) == (ssize_t)len);
    return ESP_OK;
}

static bool server_session(test_server_t *s)
{
    s->control = accept(s->listen, NULL, NULL); assert(s->control >= 0);
    atomic_store(&s->playing, false);
    s->setup_video = s->setup_audio = 0;
    uint8_t input[2048]; size_t used = 0;
    uint64_t origin = now_us(), last_video = 0, last_rtcp = 0;
    uint16_t audio_seq = 65535;
    solar_os_rtp_sender_t video_sender = {.payload_type = 96, .sequence = 1, .ssrc = 1234, .max_packet_bytes = 1200};
    uint32_t audio_ts = 0xffffff00U;
    uint8_t packet[1500], scan[80] = {0};
    solar_os_rtp_jpeg_view_t view = {.scan = scan, .scan_len = sizeof(scan), .width = 320, .height = 240, .type = 1};
    memset(view.quant_tables, 1, sizeof(view.quant_tables));
    while (!atomic_load(&s->stop)) {
        if (atomic_exchange(&s->disconnect, false)) break;
        fd_set set; FD_ZERO(&set); FD_SET(s->control, &set);
        struct timeval timeout = {.tv_usec = 1000};
        if (select(s->control + 1, &set, NULL, NULL, &timeout) > 0) {
            int n = recv(s->control, input + used, sizeof(input) - used, 0);
            if (n <= 0) break;
            used += n;
            if (solar_os_rtsp_header_length(input, used)) {
                if (used >= 4 && memcmp(input, "GET ", 4) == 0) {
                    close(s->control); s->control = -1; return false;
                }
                solar_os_rtsp_request_t r;
                if (solar_os_rtsp_parse_request(input, used, &r) != ESP_OK) {
                    fwrite(input, 1, used, stderr);
                    assert(solar_os_rtsp_parse_request(input, used, &r) == ESP_OK);
                }
                bool authorized = false;
                for (size_t i = 0; i + 14 <= used; i++)
                    if (!memcmp(input + i, "Authorization:", 14)) authorized = true;
                used = 0;
                if (atomic_load(&s->accepted) == 0 || r.method == SOLAR_OS_RTSP_METHOD_DESCRIBE)
                    atomic_fetch_add(&s->accepted, 1);
                if (r.method == SOLAR_OS_RTSP_METHOD_TEARDOWN) { atomic_store(&s->teardown, true); break; }
                if (s->require_auth && !authorized) {
                    send_response(s, r.cseq, "WWW-Authenticate: Digest realm=\"cam\", nonce=\"nonce\", qop=\"auth\"\r\n", "");
                    continue;
                }
                if (s->stall) continue;
                char headers[512] = "", body[1024] = "";
                if (r.method == SOLAR_OS_RTSP_METHOD_DESCRIBE) {
                    snprintf(headers, sizeof(headers), "Content-Base: rtsp://127.0.0.1:%u/media/\r\nContent-Type: application/sdp\r\n", s->port);
                    snprintf(body, sizeof(body), "v=0\r\na=control:*\r\n%s%s",
                        s->offer_video ? "m=video 0 RTP/AVP 96\r\na=rtpmap:96 JPEG/90000\r\na=control:trackID=0\r\n" : "",
                        s->offer_audio ? (s->relay ? "m=audio 0 RTP/AVP 97\r\na=rtpmap:97 L16/44100/1\r\na=control:trackID=1\r\n" :
                        "m=audio 0 RTP/AVP 97\r\na=rtpmap:97 L16/16000/1\r\na=control:trackID=1\r\n") : "");
                } else if (r.method == SOLAR_OS_RTSP_METHOD_SETUP) {
                    bool video = strstr(r.uri, "trackID=0") != NULL;
                    if (video) s->setup_video++; else s->setup_audio++;
                    struct sockaddr_in *peer = video ? &s->video_peer : &s->audio_peer;
                    *peer = (struct sockaddr_in){.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(r.client_rtp_port)};
                    unsigned port = video ? s->video.local_port : s->audio.local_port;
                    snprintf(headers, sizeof(headers), "Session: testing;timeout=60\r\nTransport: RTP/AVP/UDP;unicast;client_port=%u-%u;server_port=%u-%u\r\n",
                        r.client_rtp_port, r.client_rtcp_port, port, port + 1);
                } else if (r.method == SOLAR_OS_RTSP_METHOD_PLAY) {
                    assert(!strcmp(r.session, "testing")); origin = now_us();
                    atomic_store(&s->playing, true);
                }
                send_response(s, r.cseq, headers, body);
                if (s->reject || s->reject_status || (s->close_on_play && r.method == SOLAR_OS_RTSP_METHOD_PLAY)) break;
            }
        }
        if (!atomic_load(&s->playing)) continue;
        uint64_t elapsed = now_us() - origin;
        uint32_t rate = s->relay ? 44100 : 16000;
        uint32_t block = s->relay ? 1024 : 160;
        uint64_t audio_due = ((uint64_t)(uint32_t)(audio_ts - 0xffffff00U) + block) * 1000000 / rate;
        if (s->setup_audio && elapsed >= audio_due) {
            for (unsigned part = 0; part < (s->relay ? 2U : 1U); part++) {
                size_t payload = s->relay ? (part ? 660 : 1388) : 320;
                solar_os_rtp_header_t h = {.payload_type = 97, .sequence = audio_seq++, .timestamp = audio_ts, .ssrc = 5678};
                assert(solar_os_rtp_header_encode(&h, packet, sizeof(packet)) == ESP_OK);
                for (size_t i = 12; i < 12 + payload; i += 2) { packet[i] = 0x12; packet[i + 1] = 0x34; }
                assert(sendto(s->audio.rtp, packet, 12 + payload, 0, (struct sockaddr *)&s->audio_peer, sizeof(s->audio_peer)) == (ssize_t)(12 + payload));
                audio_ts += payload / 2;
            }
        }
        if (s->setup_video && elapsed >= last_video + 40000) {
            video_sender.timestamp = (uint32_t)(elapsed * 90000 / 1000000);
            assert(solar_os_rtp_jpeg_packetize(&video_sender, &view, packet, sizeof(packet), send_jpeg_packet, s) == ESP_OK);
            last_video = elapsed;
        }
        if (elapsed >= last_rtcp + 50000) {
            client_track_t *tracks[] = {&s->video, &s->audio};
            struct sockaddr_in *peers[] = {&s->video_peer, &s->audio_peer};
            for (unsigned i = 0; i < 2; i++) {
                if (!(i ? s->setup_audio : s->setup_video)) continue;
                uint32_t clock_rate = i ? rate : 90000;
                uint32_t timestamp = (uint32_t)(elapsed * clock_rate / 1000000) + (i ? 0xffffff00U : 0);
                size_t bytes;
                assert(solar_os_rtcp_sender_report(i ? 5678 : 1234, 100 + elapsed / 1000000,
                    (uint32_t)((elapsed % 1000000) * (1ULL << 32) / 1000000), timestamp,
                    10, 1000, "same-source", packet, sizeof(packet), &bytes) == ESP_OK);
                struct sockaddr_in to = *peers[i]; to.sin_port = htons(ntohs(to.sin_port) + 1);
                assert(sendto(tracks[i]->rtcp, packet, bytes, 0, (struct sockaddr *)&to, sizeof(to)) == (ssize_t)bytes);
            }
            last_rtcp = elapsed;
        }
    }
    close(s->control);
    s->control = -1;
    return true;
}

static void *server_worker(void *arg)
{
    test_server_t *s = arg;
    for (unsigned session = 0; session < (s->sessions ? s->sessions : 1);) {
        while (!atomic_load(&s->stop)) {
            fd_set set; FD_ZERO(&set); FD_SET(s->listen, &set);
            struct timeval timeout = {.tv_usec = 10000};
            if (select(s->listen + 1, &set, NULL, NULL, &timeout) > 0) break;
        }
        if (atomic_load(&s->stop)) break;
        if (!server_session(s)) continue;
        session++;
    }
    return NULL;
}

static void server_start(test_server_t *s)
{
    s->video.rtp = s->video.rtcp = s->audio.rtp = s->audio.rtcp = -1;
    assert(open_udp(NULL, &s->video) == ESP_OK && open_udp(NULL, &s->audio) == ESP_OK);
    s->listen = socket(AF_INET, SOCK_STREAM, 0); assert(s->listen >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(s->listen, (struct sockaddr *)&addr, sizeof(addr)) && !listen(s->listen, 1));
    socklen_t len = sizeof(addr); assert(!getsockname(s->listen, (struct sockaddr *)&addr, &len));
    s->port = ntohs(addr.sin_port);
    assert(!pthread_create(&s->thread, NULL, server_worker, s));
}
static void server_stop(test_server_t *s)
{
    atomic_store(&s->stop, true); pthread_join(s->thread, NULL);
    close(s->listen); close_track(&s->video); close_track(&s->audio);
}
static void samples_callback(const int16_t *samples, size_t count, uint8_t channels, void *user)
{ (void)samples; (void)user; assert(count && channels == sink_channels); atomic_fetch_add(&callbacks, 1); }
static void *run_client(void *arg) { solar_os_rtsp_client_run(arg); return NULL; }

static void test_play(bool video, bool audio, bool audio_only, bool shorthand)
{
    unsigned opens_before = atomic_load(&audio_opens);
    test_server_t server = {.offer_video = video, .offer_audio = audio}; server_start(&server);
    char address[192], url[192];
    snprintf(address, sizeof(address), "%s127.0.0.1:%u/media", shorthand ? "" : "rtsp://", server.port);
    assert(solar_os_rtsp_url_normalize(address, url, sizeof(url)) == ESP_OK);
    solar_os_rtsp_client_options_t options = {.video = !audio_only, .audio = true, .samples = samples_callback};
    solar_os_rtsp_client_t *c; assert(solar_os_rtsp_client_create(url, &options, &c) == ESP_OK);
    pthread_t thread; assert(!pthread_create(&thread, NULL, run_client, c));
    solar_os_rtsp_client_status_t status;
    uint64_t deadline = now_us() + 2000000;
    do { vTaskDelay(5); solar_os_rtsp_client_status(c, &status); } while (!status.playing && !c->cancel && now_us() < deadline);
    assert(status.playing && status.video == (video && !audio_only) && status.audio == audio);
    assert(solar_os_rtsp_client_destroy(c) == ESP_ERR_INVALID_STATE);
    assert(solar_os_rtsp_client_run(c) == ESP_ERR_INVALID_STATE);
    if (video && !audio_only) {
        solar_os_rtp_jpeg_frame_t frame;
        while (!solar_os_rtsp_client_take_video(c, &frame, NULL) && now_us() < deadline) vTaskDelay(1);
        assert(c->leased && frame.length > 100);
        uint8_t saved[128]; memcpy(saved, frame.data, sizeof(saved));
        vTaskDelay(120); assert(!memcmp(saved, frame.data, sizeof(saved)));
        assert(!solar_os_rtsp_client_take_video(c, &frame, NULL));
        solar_os_rtsp_client_release_video(c);
        while (!solar_os_rtsp_client_take_video(c, &frame, NULL) && now_us() < deadline) vTaskDelay(1);
        assert(c->leased); /* Keep the final frame leased across run() exit. */
    } else { assert(!c->jpeg); vTaskDelay(150); }
    if (audio) assert(atomic_load(&audio_opens) == opens_before + 1 && atomic_load(&samples_played) > 0 && atomic_load(&callbacks));
    else assert(atomic_load(&audio_opens) == opens_before);
    if (video && audio && !audio_only) {
        lock(c); assert(c->video.clock.valid && c->audio.clock.valid); unlock(c);
    }
    solar_os_rtsp_client_cancel(c); pthread_join(thread, NULL);
    if (video && !audio_only) {
        assert(solar_os_rtsp_client_destroy(c) == ESP_ERR_INVALID_STATE);
        solar_os_rtsp_client_release_video(c);
    }
    assert(solar_os_rtsp_client_destroy(c) == ESP_OK); server_stop(&server);
    assert(server.setup_video == (unsigned)(video && !audio_only) && server.setup_audio == (unsigned)audio);
    assert(atomic_load(&allocations) == 0 && atomic_load(&live_tasks) == 0);
    assert(atomic_load(&audio_opens) == atomic_load(&audio_closes));
    assert(atomic_load(&server.teardown));
}

static void test_failure(bool rejected, bool stalled, bool output_failed, unsigned allocation_failure)
{
    test_server_t s = {.offer_audio = true, .offer_video = allocation_failure != 0, .reject = rejected, .stall = stalled}; server_start(&s);
    atomic_store(&fail_audio, output_failed);
    char url[192]; snprintf(url, sizeof(url), "rtsp://127.0.0.1:%u/media", s.port);
    solar_os_rtsp_client_options_t options = {.audio = true, .video = allocation_failure != 0}; solar_os_rtsp_client_t *c;
    assert(solar_os_rtsp_client_create(url, &options, &c) == ESP_OK);
    atomic_store(&allocation_calls, 0); atomic_store(&fail_allocation, allocation_failure);
    pthread_t thread; pthread_create(&thread, NULL, run_client, c);
    if (stalled) { vTaskDelay(20); solar_os_rtsp_client_cancel(c); }
    pthread_join(thread, NULL);
    solar_os_rtsp_client_status_t status; solar_os_rtsp_client_status(c, &status);
    assert(status.error != ESP_OK);
    if (rejected) assert(strstr(status.error_detail, "DESCRIBE: RTSP 401") && strstr(status.error_detail, "authentication"));
    if (output_failed) assert(strstr(status.error_detail, "default audio output unavailable"));
    if (allocation_failure) assert(strstr(status.error_detail, "allocation failed"));
    solar_os_rtsp_client_destroy(c); server_stop(&s);
    atomic_store(&fail_audio, false);
    atomic_store(&fail_allocation, 0);
    assert(!atomic_load(&allocations) && !atomic_load(&live_tasks));
}

static void test_error_causes(void)
{
    const unsigned codes[] = {403, 404, 453, 454, 461};
    const char *reasons[] = {"access denied", "stream/path not found", "capacity exhausted", "session not found", "rejected UDP transport"};
    for (unsigned i = 0; i <= sizeof(codes) / sizeof(codes[0]); i++) {
        bool eof = i == sizeof(codes) / sizeof(codes[0]);
        test_server_t server = {.offer_video = true, .reject_status = eof ? 0 : codes[i], .close_on_play = eof};
        server_start(&server);
        char url[192]; snprintf(url, sizeof(url), "rtsp://127.0.0.1:%u/media", server.port);
        solar_os_rtsp_client_t *c;
        solar_os_rtsp_client_options_t options = {.video = true};
        assert(solar_os_rtsp_client_create(url, &options, &c) == ESP_OK);
        assert(solar_os_rtsp_client_run(c) != ESP_OK);
        solar_os_rtsp_client_status_t status; solar_os_rtsp_client_status(c, &status);
        assert(strstr(status.error_detail, eof ? "server closed RTSP connection" : reasons[i]));
        assert(solar_os_rtsp_client_destroy(c) == ESP_OK); server_stop(&server);
        assert(!atomic_load(&allocations) && !atomic_load(&live_tasks));
    }
    /* A bound but non-listening port deterministically refuses connections. */
    int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(fd, (struct sockaddr *)&address, sizeof(address)));
    socklen_t size = sizeof(address); assert(!getsockname(fd, (struct sockaddr *)&address, &size));
    char url[192]; snprintf(url, sizeof(url), "rtsp://127.0.0.1:%u/media", ntohs(address.sin_port));
    solar_os_rtsp_client_t *c; solar_os_rtsp_client_options_t options = {.video = true};
    assert(solar_os_rtsp_client_create(url, &options, &c) == ESP_OK);
    assert(solar_os_rtsp_client_run(c) == ESP_FAIL);
    solar_os_rtsp_client_status_t status; solar_os_rtsp_client_status(c, &status);
    assert(strstr(status.error_detail, "connect RTSP server") && strstr(status.error_detail, "errno"));
    assert(strstr(status.error_detail, strerror(ECONNREFUSED)));
    assert(solar_os_rtsp_client_destroy(c) == ESP_OK); close(fd);
}

static void test_relay_playback(uint32_t output_rate, uint8_t output_channels, uint32_t block)
{
    sink_rate = output_rate; sink_channels = output_channels; sink_block = block;
    test_server_t server = {.offer_video = true, .offer_audio = true, .relay = true};
    server_start(&server);
    char url[192]; snprintf(url, sizeof(url), "rtsp://127.0.0.1:%u/media", server.port);
    solar_os_rtsp_client_options_t options = {.video = true, .audio = true, .samples = samples_callback,
        .diagnostics = true};
    solar_os_rtsp_client_t *c; assert(solar_os_rtsp_client_create(url, &options, &c) == ESP_OK);
    unsigned before = atomic_load(&samples_played);
    pthread_t thread; assert(!pthread_create(&thread, NULL, run_client, c));
    solar_os_rtsp_client_status_t status;
    uint64_t deadline = now_us() + 2000000;
    do { vTaskDelay(1); solar_os_rtsp_client_status(c, &status); }
    while (!status.playing && !c->cancel && now_us() < deadline);
    assert(status.playing);
    deadline = now_us() + 1100000;
    unsigned frames = 0;
    while (now_us() < deadline) {
        solar_os_rtp_jpeg_frame_t frame;
        uint64_t arrived;
        if (solar_os_rtsp_client_take_video(c, &frame, &arrived)) {
            assert(arrived <= now_us());
            int64_t late = solar_os_rtsp_client_video_lateness(c, frame.timestamp, arrived);
            assert(late < 150000);
            /* Decode can be well ahead of audio, especially during startup
             * with a large native output quantum. Presentation waits remain
             * bounded even when RTSP setup queued early RTP packets. */
            if (late < -150000)
                assert(solar_os_rtsp_client_video_lateness(c, frame.timestamp, now_us() - 200000) == 0);
            frames++;
            solar_os_rtsp_client_release_video(c);
        }
        vTaskDelay(1);
    }
    solar_os_rtsp_client_status(c, &status);
    assert(status.audio_playing && status.sample_rate == 44100 && !status.audio_dropped);
    assert(status.audio_output_rate == sink_rate && status.audio_output_channels == sink_channels);
    assert(status.audio_block_frames == sink_block && status.audio_blocks > 0);
    assert(status.audio_output_frames == status.audio_blocks * sink_block);
    assert(status.audio_write_max_us > 0 && status.audio_gap_max_us > 0);
    assert(status.audio_queued <= SOLAR_OS_RTSP_AUDIO_SLOTS && !status.audio_concealed);
    lock(c); assert(!c->jitter->concealed); unlock(c); /* Silence must not hide lost large packets. */
    /* At least 800 ms of intact resampled audio. The old
     * payload cap lost most samples and cannot satisfy this assertion. */
    assert(atomic_load(&samples_played) - before >= sink_rate * sink_channels * 8 / 10 && frames >= 20);
    solar_os_rtsp_client_cancel(c); pthread_join(thread, NULL);
    assert(solar_os_rtsp_client_destroy(c) == ESP_OK); server_stop(&server);
    assert(!atomic_load(&allocations) && !atomic_load(&live_tasks));
    sink_rate = 48000; sink_channels = 2; sink_block = 480;
}

static void test_reconnect(void)
{
    test_server_t server = {.offer_video = true, .offer_audio = true, .sessions = 4};
    server_start(&server);
    char url[192]; snprintf(url, sizeof(url), "rtsp://127.0.0.1:%u/media", server.port);
    solar_os_rtsp_client_options_t options = {.video = true, .audio = true,
        .diagnostics = true, .reconnect_attempts = 3};
    solar_os_rtsp_client_t *c; assert(solar_os_rtsp_client_create(url, &options, &c) == ESP_OK);
    pthread_t thread; assert(!pthread_create(&thread, NULL, run_client, c));
    for (unsigned epoch = 1; epoch <= 3; epoch++) {
        solar_os_rtp_jpeg_frame_t frame;
        solar_os_rtsp_client_status_t status;
        uint64_t deadline = now_us() + 5000000;
        bool got = false;
        while (now_us() < deadline) {
            solar_os_rtsp_client_status(c, &status);
            if (status.epoch == epoch && status.playing && status.audio_playing &&
                solar_os_rtsp_client_take_video(c, &frame, NULL)) { got = true; break; }
            vTaskDelay(1);
        }
        assert(got && frame.length > 80 && status.audio_stack_min_free == 4096);
        assert(status.error == ESP_OK && status.reconnects == epoch - 1);
        if (epoch == 3) { solar_os_rtsp_client_release_video(c); break; }
        atomic_store(&server.disconnect, true);
        deadline = now_us() + 1000000;
        do { vTaskDelay(1); solar_os_rtsp_client_status(c, &status); }
        while (!status.reconnecting && now_us() < deadline);
        if (!status.reconnecting || status.playing)
            fprintf(stderr, "reconnect epoch=%u error=%d %s cancel=%d retry=%d done=%d\n",
                epoch, status.error, status.error_detail, c->cancel, c->retryable, c->audio_done);
        assert(status.reconnecting && !status.playing);
        /* An outstanding decoder lease must survive an entire retry backoff. */
        uint8_t byte = frame.data[0];
        vTaskDelay(1100);
        solar_os_rtsp_client_status(c, &status);
        assert(status.epoch == epoch && frame.data[0] == byte);
        solar_os_rtsp_client_release_video(c);
    }
    solar_os_rtsp_client_cancel(c); pthread_join(thread, NULL);
    assert(solar_os_rtsp_client_destroy(c) == ESP_OK); server_stop(&server);
    assert(!atomic_load(&allocations) && !atomic_load(&live_tasks));

    const unsigned codes[] = {403, 404};
    for (unsigned i = 0; i < 2; i++) {
        server = (test_server_t){.offer_video = true, .reject_status = codes[i], .sessions = 3};
        server_start(&server);
        snprintf(url, sizeof(url), "rtsp://127.0.0.1:%u/media", server.port);
        options.reconnect_attempts = 2;
        assert(solar_os_rtsp_client_create(url, &options, &c) == ESP_OK);
        assert(solar_os_rtsp_client_run(c) == ESP_ERR_INVALID_RESPONSE);
        solar_os_rtsp_client_status_t status; solar_os_rtsp_client_status(c, &status);
        assert(status.reconnects == (i ? 2U : 0U) && !status.reconnecting);
        assert(atomic_load(&server.accepted) == (i ? 3U : 1U));
        assert(strstr(status.error_detail, i ? "404" : "403"));
        assert(solar_os_rtsp_client_destroy(c) == ESP_OK); server_stop(&server);
    }
    server = (test_server_t){.offer_video = true, .reject_status = 404, .sessions = 3};
    server_start(&server);
    snprintf(url, sizeof(url), "rtsp://127.0.0.1:%u/media", server.port);
    assert(solar_os_rtsp_client_create(url, &options, &c) == ESP_OK);
    assert(!pthread_create(&thread, NULL, run_client, c));
    uint64_t deadline = now_us() + 1000000;
    solar_os_rtsp_client_status_t status;
    do { vTaskDelay(1); solar_os_rtsp_client_status(c, &status); }
    while (!status.reconnecting && now_us() < deadline);
    assert(status.reconnecting);
    uint64_t before = now_us();
    solar_os_rtsp_client_cancel(c); pthread_join(thread, NULL);
    assert(now_us() - before < 200000);
    assert(solar_os_rtsp_client_destroy(c) == ESP_OK); server_stop(&server);
    assert(!atomic_load(&allocations) && !atomic_load(&live_tasks));
}

static void test_presentation_clock(void)
{
    solar_os_rtsp_client_options_t options = {.video = true, .audio = true};
    solar_os_rtsp_client_t *c;
    assert(solar_os_rtsp_client_create("rtsp://127.0.0.1/media", &options, &c) == ESP_OK);
    uint64_t now = now_us();
    int64_t late = solar_os_rtsp_client_video_lateness(c, 0, now);
    assert(late >= -(int64_t)SOLAR_OS_RTSP_JITTER_US && late < -70000);
    assert(solar_os_rtsp_client_video_lateness(c, 0, now - 100000) >= 20000);
    c->description.audio.present = true;
    c->description.audio.media.clock_rate = 44100;
    c->audio.clock = c->video.clock = (solar_os_rtsp_sender_clock_t){.valid = true, .ntp_us = 100000000};
    c->audio_timestamp = 4410; c->audio_played_us = now_us(); c->status.audio_playing = true;
    late = solar_os_rtsp_client_video_lateness(c, 18000, now_us());
    assert(late >= -100000 && late < -90000); /* Decode ahead; don't display yet. */
    assert(solar_os_rtsp_client_video_lateness(c, 18000, now_us() - 200000) == 0);
    c->audio_timestamp = 22050;
    assert(solar_os_rtsp_client_video_lateness(c, 18000, now_us()) >= 300000); /* Drop late video. */
    c->audio_played_us = now_us() - 200000;
    late = solar_os_rtsp_client_video_lateness(c, 18000, now_us());
    assert(late >= -(int64_t)SOLAR_OS_RTSP_JITTER_US && late < -70000); /* Stalled audio: fallback. */
    assert(solar_os_rtsp_client_destroy(c) == ESP_OK && !atomic_load(&allocations));
}

static int probe_stream(const char *url)
{
    live_probe = true;
    solar_os_rtsp_client_options_t options = {.video = true, .audio = true, .samples = samples_callback};
    solar_os_rtsp_client_t *c;
    assert(solar_os_rtsp_client_create(url, &options, &c) == ESP_OK);
    pthread_t thread; assert(!pthread_create(&thread, NULL, run_client, c));
    unsigned frames = 0;
    uint64_t deadline = now_us() + 5000000;
    while (now_us() < deadline && !c->cancel) {
        solar_os_rtp_jpeg_frame_t frame;
        if (solar_os_rtsp_client_take_video(c, &frame, NULL)) {
            assert(frame.length > 100); frames++;
            solar_os_rtsp_client_release_video(c);
        }
        vTaskDelay(1);
    }
    solar_os_rtsp_client_status_t status; solar_os_rtsp_client_status(c, &status);
    printf("Host probe (mock audio sink): frames=%u video_dropped=%u audio_dropped=%u "
           "audio=%uHz/%uch output_samples=%u error=%d\n", frames,
           status.video_dropped, status.audio_dropped, status.sample_rate, status.channels,
           atomic_load(&samples_played), status.error);
    solar_os_rtsp_client_cancel(c); pthread_join(thread, NULL);
    assert(solar_os_rtsp_client_destroy(c) == ESP_OK);
    assert(!atomic_load(&allocations) && !atomic_load(&live_tasks));
    return status.error == ESP_OK && frames > 0 && atomic_load(&samples_played) > 0 ? 0 : 1;
}

static void test_digest_auth(void)
{
    test_server_t server = {.offer_video = true, .require_auth = true};
    server_start(&server);
    char url[192];
    snprintf(url, sizeof(url), "rtsp://user:secret@127.0.0.1:%u/media", server.port);
    solar_os_rtsp_client_t *c;
    solar_os_rtsp_client_options_t options = {.video = true};
    assert(solar_os_rtsp_client_create(url, &options, &c) == ESP_OK);
    assert(!strstr(c->url, "secret") && strstr(c->url, "127.0.0.1"));
    pthread_t thread; assert(!pthread_create(&thread, NULL, run_client, c));
    solar_os_rtsp_client_status_t status;
    uint64_t deadline = now_us() + 2000000;
    do { vTaskDelay(5); solar_os_rtsp_client_status(c, &status); }
    while (!status.playing && !c->cancel && now_us() < deadline);
    assert(status.playing);
    solar_os_rtsp_client_cancel(c); pthread_join(thread, NULL);
    assert(solar_os_rtsp_client_destroy(c) == ESP_OK); server_stop(&server);
    assert(!atomic_load(&allocations) && !atomic_load(&live_tasks));

    server = (test_server_t){.offer_video = true};
    server_start(&server);
    assert(solar_os_rtsp_auth_set("127.0.0.1", server.port, "user", "secret") == ESP_OK);
    snprintf(url, sizeof(url), "rtsp://127.0.0.1:%u/media", server.port);
    server.require_auth = true;
    assert(solar_os_rtsp_client_create(url, &options, &c) == ESP_OK);
    assert(!strcmp(c->user, "user"));
    assert(!pthread_create(&thread, NULL, run_client, c));
    deadline = now_us() + 2000000;
    do { vTaskDelay(5); solar_os_rtsp_client_status(c, &status); }
    while (!status.playing && !c->cancel && now_us() < deadline);
    assert(status.playing);
    solar_os_rtsp_client_cancel(c); pthread_join(thread, NULL);
    assert(solar_os_rtsp_client_destroy(c) == ESP_OK); server_stop(&server);
    assert(solar_os_rtsp_auth_clear(NULL, 0) == ESP_OK);
}

int main(int argc, char **argv)
{
    if (argc == 2) return probe_stream(argv[1]);
    assert(argc == 1);
    for (unsigned i = 1; i <= 3; i++) {
        atomic_store(&allocation_calls, 0); atomic_store(&fail_allocation, i);
        solar_os_rtsp_client_t *c = NULL;
        solar_os_rtsp_client_options_t o = {.audio = true};
        assert(solar_os_rtsp_client_create("rtsp://127.0.0.1/media", &o, &c) == ESP_ERR_NO_MEM && !c);
        assert(!atomic_load(&allocations));
    }
    atomic_store(&fail_allocation, 0);
    test_presentation_clock();
    test_play(true, false, false, false); test_play(false, true, false, false);
    test_play(true, true, false, false); test_play(true, true, true, false);
    test_play(true, true, false, true); test_play(true, true, true, true);
    test_relay_playback(48000, 2, 480);
    test_relay_playback(16000, 1, 512);
    test_failure(true, false, false, 0); test_failure(false, true, false, 0); test_failure(false, false, true, 0);
    for (unsigned i = 1; i <= 3; i++) test_failure(false, false, false, i);
    test_error_causes();
    test_digest_auth();
    test_reconnect();
    puts("rtsp_client_test: OK"); return 0;
}
