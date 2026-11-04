/* Real publisher/RTSP/RTP code; POSIX sockets/tasks and bounded fake sources. */
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/socket.h>

static ssize_t test_sendto(int, const void *, size_t, int,
                            const struct sockaddr *, socklen_t);
#define sendto test_sendto

#include "../../src/jobs/solar_os_rtspd_job.c"
#undef sendto

static unsigned latency_leases;
esp_err_t solar_os_wifi_latency_acquire(const char *owner)
{ assert(!strcmp(owner, RTSPD_OWNER) && latency_leases++ == 0); return ESP_OK; }
esp_err_t solar_os_wifi_latency_release(const char *owner)
{ assert(!strcmp(owner, RTSPD_OWNER) && latency_leases-- == 1); return ESP_OK; }

static atomic_bool force_congestion;
static ssize_t test_sendto(int fd, const void *data, size_t len, int flags,
                            const struct sockaddr *peer, socklen_t peer_len)
{
    const uint8_t *packet = data;
    if (len >= 12 && (packet[1] & 127) == 97 && atomic_exchange(&force_congestion, false)) {
        errno = ENOMEM;
        return -1;
    }
    return sendto(fd, data, len, flags, peer, peer_len);
}

struct test_task { pthread_t thread; TaskFunction_t fn; void *arg; };
static unsigned task_calls, fail_task_call, live_tasks;
static unsigned allocation_calls, fail_allocation_call, live_allocations;
static size_t live_bytes;
static bool force_wait_timeout;
static unsigned camera_claims, camera_releases;
static bool camera_busy, audio_busy;
static unsigned audio_opens, audio_closes;
static pthread_t audio_owner;
static uint8_t jpeg_buffer[2048];
static solar_os_camera_frame_t fake_frame;

typedef struct { size_t size; uint32_t guard; } test_allocation_t;
static const uint32_t allocation_guard = 0x5a5aa5a5U;
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t kind, const char *tag)
{
    assert(kind == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED);
    assert(!strncmp(tag, "rtspd.", 6));
    if (++allocation_calls == fail_allocation_call) return NULL;
    test_allocation_t *allocation = malloc(sizeof(*allocation) + size + sizeof(allocation_guard));
    assert(allocation);
    *allocation = (test_allocation_t){.size = size, .guard = allocation_guard};
    void *data = allocation + 1;
    memcpy((uint8_t *)data + size, &allocation_guard, sizeof(allocation_guard));
    live_allocations++; live_bytes += size;
    return data;
}
void *solar_os_memory_calloc(size_t count, size_t size,
                            solar_os_memory_class_t kind, const char *tag)
{
    void *data = solar_os_memory_alloc(count * size, kind, tag);
    if (data) memset(data, 0, count * size);
    return data;
}
void solar_os_memory_free(void *data)
{
    if (!data) return;
    test_allocation_t *allocation = (test_allocation_t *)data - 1;
    uint32_t tail;
    memcpy(&tail, (uint8_t *)data + allocation->size, sizeof(tail));
    assert(allocation->guard == allocation_guard && tail == allocation_guard);
    live_allocations--; live_bytes -= allocation->size;
    free(allocation);
}

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000000LL + now.tv_nsec / 1000;
}
void vTaskDelay(TickType_t ticks)
{
    struct timespec delay = {.tv_sec = ticks / 1000U,
                             .tv_nsec = (ticks % 1000U) * 1000000L};
    nanosleep(&delay, NULL);
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{ assert(!task); return 3072U; }
uint32_t esp_random(void) { static uint32_t value = 1000; return ++value; }
const char *esp_err_to_name(esp_err_t error)
{ (void)error; return "ESP_ERR_INVALID_RESPONSE"; }
static void *task_entry(void *arg)
{
    struct test_task *task = arg;
    task->fn(task->arg);
    return NULL;
}
BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t fn, const char *name,
    uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *handle,
    BaseType_t core, solar_os_task_role_t role)
{
    (void)name; (void)stack; (void)priority; (void)core; (void)role;
    if (++task_calls == fail_task_call) return 0;
    struct test_task *task = calloc(1, sizeof(*task));
    assert(task);
    task->fn = fn; task->arg = arg;
    *handle = task;
    assert(!pthread_create(&task->thread, NULL, task_entry, task));
    live_tasks++;
    return pdPASS;
}
void solar_os_task_delete_internal(TaskHandle_t task)
{
    assert(!task);
    pthread_exit(NULL);
}
bool solar_os_task_wait_done(TaskHandle_t task, volatile bool *done, uint32_t timeout)
{
    if (!task) return true;
    if (force_wait_timeout) return false;
    const int64_t deadline = esp_timer_get_time() + timeout * 1000LL;
    while (!*done && esp_timer_get_time() < deadline) vTaskDelay(1);
    if (!*done) return false;
    assert(!pthread_join(task->thread, NULL));
    free(task); live_tasks--;
    return true;
}
solar_os_shell_io_t *solar_os_context_shell_io(solar_os_context_t *ctx)
{ static solar_os_shell_io_t io; return ctx ? &io : NULL; }
int solar_os_shell_io_printf(solar_os_shell_io_t *io, const char *fmt, ...)
{
    (void)io;
    char buffer[192]; va_list args; va_start(args, fmt);
    int length = vsnprintf(buffer, sizeof(buffer), fmt, args); va_end(args);
    assert(length >= 0 && (size_t)length < sizeof(buffer));
    return 0;
}
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{ (void)level; (void)tag; (void)fmt; return ESP_OK; }
esp_err_t solar_os_jobs_note_resource(const char *job, solar_os_job_resource_type_t type,
                                      const char *name, const char *detail)
{ (void)type; (void)name; (void)detail; assert(!strcmp(job, "rtspd")); return ESP_OK; }
esp_err_t solar_os_jobs_get_generation(const char *job, uint32_t *generation)
{ (void)job; *generation = 1; return ESP_OK; }
esp_err_t solar_os_jobs_mark_stopped(const char *job, uint32_t generation, esp_err_t error)
{ (void)job; (void)generation; (void)error; return ESP_OK; }

esp_err_t solar_os_stream_get_info(const char *id, solar_os_stream_info_t *info)
{
    if (!strcmp(id, "missing")) return ESP_ERR_NOT_FOUND;
    if (!strcmp(id, "camera0")) {
#if SOLAR_OS_PACKAGE_SERVICE_CAMERA
        *info = (solar_os_stream_info_t){.type = SOLAR_OS_STREAM_TYPE_VIDEO,
            .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
            .video = {.codec = SOLAR_OS_STREAM_VIDEO_JPEG, .width = 320, .height = 240}};
        return ESP_OK;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
    *info = (solar_os_stream_info_t) {
        .type = SOLAR_OS_STREAM_TYPE_AUDIO,
        .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
        .audio = {.sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
                  .sample_rate = 16000, .channels = 1, .bits_per_sample = 16},
    };
    if (!strcmp(id, "sink")) info->direction = SOLAR_OS_STREAM_DIRECTION_SINK;
    if (!strcmp(id, "scalar")) info->type = SOLAR_OS_STREAM_TYPE_SCALAR;
    if (!strcmp(id, "badformat")) info->audio.bits_per_sample = 24;
    return ESP_OK;
}
esp_err_t solar_os_stream_open_ex(const char *id, const char *owner,
    const solar_os_stream_open_options_t *options, solar_os_stream_handle_t *handle)
{
    assert(!strcmp(owner, "job:rtspd"));
    assert(options->direction == SOLAR_OS_STREAM_DIRECTION_SOURCE);
    if (!strcmp(id, "camera0")) {
        solar_os_camera_owner_t token = {0};
        esp_err_t error = solar_os_camera_acquire(owner, &token);
        if (error != ESP_OK) return error;
        const solar_os_camera_config_t config = {
            .frame_size = options->requested_video.width == 640 ?
                SOLAR_OS_CAMERA_FRAME_SIZE_VGA : SOLAR_OS_CAMERA_FRAME_SIZE_QVGA,
            .jpeg_quality = options->requested_video.jpeg_quality,
        };
        assert(solar_os_camera_start(&token, &config) == ESP_OK);
        handle->private_data[0] = token.generation;
        handle->type = SOLAR_OS_STREAM_TYPE_VIDEO;
        handle->video = options->requested_video;
        handle->slot = 0;
        return ESP_OK;
    }
    assert(!strcmp(id, "mic0"));
    if (audio_busy) return ESP_ERR_INVALID_STATE;
    audio_owner = pthread_self();
    handle->slot = 1;
    handle->audio = options->requested_audio;
    handle->context = (void *)1;
    audio_opens++;
    return ESP_OK;
}
esp_err_t solar_os_stream_read(solar_os_stream_handle_t *handle, void *data,
    size_t len, uint32_t timeout, size_t *read_len)
{
    (void)timeout;
    assert(handle->context && pthread_equal(audio_owner, pthread_self()));
    vTaskDelay((TickType_t)(len / 2U * 1000U / handle->audio.sample_rate));
    for (size_t i = 0; i < len / 2U; i++) ((int16_t *)data)[i] = 0x1234;
    *read_len = len;
    return ESP_OK;
}
void solar_os_stream_close(solar_os_stream_handle_t *handle)
{
    if (!handle->context) return;
    assert(pthread_equal(audio_owner, pthread_self()));
    audio_closes++; handle->context = NULL;
}

esp_err_t solar_os_stream_close_ex(solar_os_stream_handle_t *handle)
{
    if (handle->slot < 0 || handle->leased_frame != NULL) return ESP_ERR_INVALID_STATE;
    if (handle->type == SOLAR_OS_STREAM_TYPE_VIDEO) {
        solar_os_camera_owner_t token = {.generation = handle->private_data[0]};
        assert(solar_os_camera_stop(&token) == ESP_OK);
        assert(solar_os_camera_release_owner(&token) == ESP_OK);
    } else solar_os_stream_close(handle);
    *handle = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
    return ESP_OK;
}

esp_err_t solar_os_stream_acquire_frame(solar_os_stream_handle_t *handle,
                                       solar_os_stream_video_frame_t *frame)
{
    assert(handle->slot == 0 && handle->leased_frame == NULL);
    const solar_os_camera_owner_t token = {.generation = handle->private_data[0]};
    const solar_os_camera_frame_t *captured;
    assert(solar_os_camera_capture(&token, &captured) == ESP_OK);
    *frame = (solar_os_stream_video_frame_t){.data = captured->data,
        .length = captured->length, .width = captured->width, .height = captured->height,
        .timestamp_us = captured->timestamp_us};
    handle->leased_frame = frame;
    return ESP_OK;
}

esp_err_t solar_os_stream_release_frame(solar_os_stream_handle_t *handle,
                                       solar_os_stream_video_frame_t *frame)
{
    assert(handle->leased_frame == frame);
    const solar_os_camera_owner_t token = {.generation = handle->private_data[0]};
    assert(solar_os_camera_release_frame(&token, &fake_frame) == ESP_OK);
    handle->leased_frame = NULL;
    memset(frame, 0, sizeof(*frame));
    return ESP_OK;
}

esp_err_t solar_os_camera_acquire(const char *owner, solar_os_camera_owner_t *token)
{
    assert(!strcmp(owner, "job:rtspd"));
    if (camera_busy) return ESP_ERR_INVALID_STATE;
    token->generation = 1; camera_claims++; return ESP_OK;
}
esp_err_t solar_os_camera_start(const solar_os_camera_owner_t *token,
                                const solar_os_camera_config_t *config)
{ (void)config; assert(token->generation); return ESP_OK; }
esp_err_t solar_os_camera_capture(const solar_os_camera_owner_t *token,
                                  const solar_os_camera_frame_t **frame)
{
    assert(token->generation); vTaskDelay(30);
    fake_frame.timestamp_us = (uint64_t)esp_timer_get_time();
    *frame = &fake_frame; return ESP_OK;
}
esp_err_t solar_os_camera_release_frame(const solar_os_camera_owner_t *token,
                                        const solar_os_camera_frame_t *frame)
{ assert(token->generation && frame == &fake_frame); return ESP_OK; }
esp_err_t solar_os_camera_stop(const solar_os_camera_owner_t *token)
{ assert(token->generation); return ESP_OK; }
esp_err_t solar_os_camera_release_owner(solar_os_camera_owner_t *token)
{ assert(token->generation); token->generation = 0; camera_releases++; return ESP_OK; }

static esp_err_t fixture_packet(const uint8_t *packet, size_t len, void *user)
{
    solar_os_rtp_jpeg_receiver_t *receiver = user;
    solar_os_rtp_jpeg_frame_t frame;
    assert(solar_os_rtp_jpeg_receiver_feed(receiver, packet, len, &frame) == ESP_OK);
    assert(frame.length);
    fake_frame.data = frame.data; fake_frame.length = frame.length;
    return ESP_OK;
}
static void make_jpeg(void)
{
    uint8_t scan[100] = {1}, packet[1200];
    solar_os_rtp_jpeg_view_t view = {.scan = scan, .scan_len = sizeof(scan),
                                    .width = 320, .height = 240, .type = 0};
    memset(view.quant_tables, 1, sizeof(view.quant_tables));
    solar_os_rtp_sender_t sender = {.payload_type = 96, .max_packet_bytes = 1200};
    solar_os_rtp_jpeg_receiver_t receiver;
    assert(solar_os_rtp_jpeg_receiver_init(&receiver, 96, jpeg_buffer,
                                          sizeof(jpeg_buffer)) == ESP_OK);
    assert(solar_os_rtp_jpeg_packetize(&sender, &view, packet, sizeof(packet),
                                      fixture_packet, &receiver) == ESP_OK);
}

static void options_test(void)
{
    solar_os_rtspd_options_t options;
    assert(solar_os_rtspd_parse_options(0, NULL, &options));
    assert(options.video && !options.audio[0] && options.fps == 5 && options.port == 554);
    assert(!strcmp(options.video_source, "camera0"));
    char *audio[] = {"rtspd", "video=none", "audio=mic0", "port=8554"};
    assert(solar_os_rtspd_parse_options(4, audio, &options));
    assert(!options.video && !strcmp(options.audio, "mic0"));
    char *both[] = {"video=camera", "audio=mic0", "size=vga", "fps=0"};
    assert(solar_os_rtspd_parse_options(4, both, &options));
    assert(options.fps == 0 && options.camera.frame_size == SOLAR_OS_CAMERA_FRAME_SIZE_VGA);
    assert(!strcmp(options.video_source, "camera0"));
    char *explicit_video[] = {"video=mycam"};
    assert(solar_os_rtspd_parse_options(1, explicit_video, &options));
    assert(!strcmp(options.video_source, "mycam"));
    char *none[] = {"video=none", "audio=none"};
    assert(!solar_os_rtspd_parse_options(2, none, &options));
    char *duplicate[] = {"audio=mic0", "audio=none"};
    assert(!solar_os_rtspd_parse_options(2, duplicate, &options));
    char *audio_size[] = {"video=none", "audio=mic0", "size=qvga"};
    assert(!solar_os_rtspd_parse_options(3, audio_size, &options));
    const char *invalid[] = {"fps=31", "fps=-1", "fps=1x", "fps=", "port=0",
        "port=65536", "audio=", "audio=mic/0", "video=", "video=cam/0", "size=qqvga", "5", "wat=1"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char *arg = (char *)invalid[i];
        assert(!solar_os_rtspd_parse_options(1, &arg, &options));
    }
    assert(!solar_os_rtspd_video_due(999, 1000, 0, 5));
    assert(solar_os_rtspd_video_due(1000, 1000, 0, 5));
    assert(!solar_os_rtspd_video_due(101000, 1000, 1000, 5));
    assert(solar_os_rtspd_video_due(201000, 1000, 1000, 5));
    assert(solar_os_rtspd_video_due(2000, 1000, 1000, 0));
}

static int bound_udp(uint16_t *port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0); assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(fd, (struct sockaddr *)&address, sizeof(address)));
    socklen_t len = sizeof(address); assert(!getsockname(fd, (struct sockaddr *)&address, &len));
    *port = ntohs(address.sin_port);
    struct timeval timeout = {.tv_sec = 1};
    assert(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    return fd;
}
static int connect_control(void)
{
    struct sockaddr_in address;
    socklen_t len = sizeof(address);
    assert(!getsockname(rtspd.listen_fd, (struct sockaddr *)&address, &len));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
    assert(!connect(fd, (struct sockaddr *)&address, sizeof(address)));
    struct timeval timeout = {.tv_sec = 1};
    assert(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    return fd;
}
static int udp_pair(uint16_t *port, int *rtcp)
{
    for (unsigned attempt = 0; attempt < 128; attempt++) {
        int rtp = bound_udp(port);
        *rtcp = socket(AF_INET, SOCK_DGRAM, 0); assert(*rtcp >= 0);
        struct sockaddr_in addr = {.sin_family = AF_INET,
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
            .sin_port = htons((uint16_t)(*port + 1U))};
        if (*port < 65535 && !bind(*rtcp, (struct sockaddr *)&addr, sizeof(addr))) {
            struct timeval timeout = {.tv_sec = 6};
            assert(!setsockopt(*rtcp, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
            return rtp;
        }
        close(rtp); close(*rtcp);
    }
    assert(!"unable to bind RTP/RTCP pair"); return -1;
}
static char response[2048];
static unsigned cseq;
static void request(int fd, const char *method, const char *uri,
                      const char *headers, int expected)
{
    char text[1024];
    int n = snprintf(text, sizeof(text), "%s %s RTSP/1.0\r\nCSeq: %u\r\n%s\r\n",
                     method, uri, ++cseq, headers ? headers : "");
    assert(send(fd, text, (size_t)n, 0) == n);
    size_t used = 0;
    response[0] = '\0';
    for (;;) {
        ssize_t got = recv(fd, response + used, sizeof(response) - used - 1, 0);
        assert(got > 0); used += (size_t)got; response[used] = '\0';
        char *end = strstr(response, "\r\n\r\n");
        if (!end) continue;
        char *length = strstr(response, "Content-Length:");
        size_t body = length ? strtoul(length + 15, NULL, 10) : 0;
        if (used >= (size_t)(end + 4 - response) + body) break;
    }
    int code = 0; assert(sscanf(response, "RTSP/1.0 %d", &code) == 1 && code == expected);
}
static void setup(int fd, unsigned track, uint16_t port)
{
    char uri[64], headers[128];
    snprintf(uri, sizeof(uri), "rtsp://127.0.0.1/media/trackID=%u", track);
    snprintf(headers, sizeof(headers),
              "Transport: RTP/AVP/UDP;unicast;client_port=%u-%u\r\n", port, port + 1);
    request(fd, "SETUP", uri, headers, 200);
}
static void free_control_port(char *port, size_t capacity)
{
    int temp = socket(AF_INET, SOCK_STREAM, 0); assert(temp >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET}; socklen_t len = sizeof(addr);
    assert(!bind(temp, (struct sockaddr *)&addr, len)); assert(!getsockname(temp, (struct sockaddr *)&addr, &len));
    snprintf(port, capacity, "port=%u", ntohs(addr.sin_port)); close(temp);
}
static void run_sessions(bool video)
{
    char port[32]; free_control_port(port, sizeof(port));
    char *args[] = {video ? "video=camera" : "video=none", "audio=mic0", port, "fps=0"};
    unsigned camera_before = camera_claims;
    assert(solar_os_rtspd_job.start(NULL, video ? 4 : 3, args) == ESP_OK);
    assert(latency_leases == 1);
    assert(live_allocations == (video ? 3U : 2U));
    assert(live_bytes == sizeof(rtspd_session_t) + sizeof(rtspd_audio_scratch_t) +
                         (video ? sizeof(rtspd_video_scratch_t) : 0U));
    assert((rtspd.sources[RTSPD_VIDEO].scratch != NULL) == video);
    for (unsigned round = 0; round < 2; round++) {
        uint16_t audio_port, video_port;
        int audio_rtcp, video_rtcp;
        int audio_fd = udp_pair(&audio_port, &audio_rtcp);
        int video_fd = udp_pair(&video_port, &video_rtcp);
        int fd = connect_control();
        request(fd, "DESCRIBE", "rtsp://127.0.0.1/media", "Accept: application/sdp\r\n", 200);
        assert(strstr(response, "L16/16000/1"));
        assert((strstr(response, "JPEG/90000") != NULL) == video);
        if (!video) request(fd, "SETUP", "rtsp://127.0.0.1/media/trackID=0",
            "Transport: RTP/AVP/UDP;unicast;client_port=40000-40001\r\n", 454);
        if (video) setup(fd, RTSPD_VIDEO, video_port);
        setup(fd, RTSPD_AUDIO, audio_port);
        request(fd, "PLAY", "rtsp://127.0.0.1/media", "Session: wrong\r\n", 454);
        request(fd, "PLAY", "rtsp://127.0.0.1/media", NULL, 200);
        assert(strstr(response, "trackID=1;seq="));
        if (video) assert(strstr(response, "trackID=0;seq="));
        request(fd, "PLAY", "rtsp://127.0.0.1/media", NULL, 455);
        request(fd, "SETUP", "rtsp://127.0.0.1/media/trackID=1",
            "Transport: RTP/AVP/UDP;unicast;client_port=40000-40001\r\n", 455);
        uint8_t packet[1200];
        uint32_t timestamp = 0;
        for (unsigned i = 0; i < 4; i++) {
            ssize_t n = recv(audio_fd, packet, sizeof(packet), 0); assert(n == 652);
            assert((packet[1] & 127) == 97 && packet[12] == 0x12 && packet[13] == 0x34);
            uint32_t current; memcpy(&current, packet + 4, 4); current = ntohl(current);
            if (i == 1) assert(current - timestamp >= 640 && current - timestamp <= 1600);
            else if (i) assert(current - timestamp == 320);
            if (!i) atomic_store(&force_congestion, true);
            timestamp = current;
        }
        if (video) {
            uint32_t previous = 0;
            for (unsigned i = 0; i < 3; i++) {
                assert(recv(video_fd, packet, sizeof(packet), 0) > 20);
                assert((packet[1] & 127) == 96);
                uint32_t current; memcpy(&current, packet + 4, 4); current = ntohl(current);
                if (i) assert(current - previous >= 1800 && current - previous < 9000);
                previous = current;
            }
        }
        int rejected = connect_control();
        ssize_t n = recv(rejected, packet, sizeof(packet) - 1, 0); assert(n > 0); packet[n] = 0;
        assert(strstr((char *)packet, "453 Not Enough Bandwidth")); close(rejected);
        if (!round) {
            ssize_t report_len = recv(audio_rtcp, packet, sizeof(packet), 0);
            assert(report_len > 40 && packet[1] == 200 && packet[29] == 202);
            size_t cname_len = packet[37]; assert(cname_len < 48);
            uint8_t cname[48]; memcpy(cname, packet + 38, cname_len);
            if (video) {
                report_len = recv(video_rtcp, packet, sizeof(packet), 0);
                assert(report_len > 40 && packet[1] == 200 && packet[29] == 202);
                assert(packet[37] == cname_len && !memcmp(cname, packet + 38, cname_len));
            }
        }
        if (!round) request(fd, "TEARDOWN", "rtsp://127.0.0.1/media", NULL, 200);
        close(fd); close(audio_fd); close(video_fd);
        close(audio_rtcp); close(video_rtcp); vTaskDelay(150);
    }
    uint16_t active_port;
    int active_udp = bound_udp(&active_port), active_control = connect_control();
    setup(active_control, RTSPD_AUDIO, active_port);
    request(active_control, "PLAY", "rtsp://127.0.0.1/media", NULL, 200);
    uint8_t active_packet[1200];
    assert(recv(active_udp, active_packet, sizeof(active_packet), 0) > 12);
    solar_os_rtspd_job.stop(NULL);
    assert(recv(active_control, active_packet, sizeof(active_packet), 0) == 0);
    close(active_control); close(active_udp);
    assert(!rtspd.running && !live_tasks && !rtspd.session);
    assert(!rtspd.session && !live_allocations && !live_bytes);
    assert(!rtspd.sources[0].scratch && !rtspd.sources[1].scratch);
    assert(rtspd.sources[RTSPD_AUDIO].stack_min_free == 3072U);
    if (video) assert(rtspd.sources[RTSPD_VIDEO].stack_min_free == 3072U);
    assert(audio_opens == audio_closes);
    assert(camera_claims == camera_before + (video ? 1U : 0U));
    assert(camera_claims == camera_releases);
}

static void failure_tests(void)
{
    const char *bad[] = {"audio=missing", "audio=sink", "audio=scalar", "audio=badformat"};
    unsigned camera_before = camera_claims;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char *args[] = {"video=none", (char *)bad[i]};
        assert(solar_os_rtspd_job.start(NULL, 2, args) != ESP_OK);
        assert(!live_tasks && camera_claims == camera_before);
    }
    char port[32]; free_control_port(port, sizeof(port));
    char *audio[] = {"video=none", "audio=mic0", port};
    for (unsigned fail = 1; fail <= 2; fail++) {
        fail_allocation_call = allocation_calls + fail;
        assert(solar_os_rtspd_job.start(NULL, 3, audio) == ESP_ERR_NO_MEM);
        assert(!live_tasks && !live_allocations && !live_bytes && !rtspd.session);
        assert(camera_claims == camera_before);
        fail_allocation_call = 0;
    }
    audio_busy = true;
    assert(solar_os_rtspd_job.start(NULL, 3, audio) == ESP_ERR_INVALID_STATE);
    audio_busy = false;
    assert(!live_tasks && audio_opens == audio_closes);
    for (unsigned fail = 1; fail <= 2; fail++) {
        fail_task_call = task_calls + fail;
        assert(solar_os_rtspd_job.start(NULL, 3, audio) == ESP_ERR_NO_MEM);
        assert(!live_tasks && audio_opens == audio_closes);
        assert(!live_allocations && !rtspd.session);
        fail_task_call = 0;
    }
#if SOLAR_OS_PACKAGE_SERVICE_CAMERA
    char *both[] = {"video=camera", "audio=mic0", port};
    for (unsigned fail = 1; fail <= 3; fail++) {
        fail_allocation_call = allocation_calls + fail;
        assert(solar_os_rtspd_job.start(NULL, 3, both) == ESP_ERR_NO_MEM);
        assert(!live_tasks && !live_allocations && !live_bytes && !rtspd.session);
        assert(camera_claims == camera_before);
        fail_allocation_call = 0;
    }
    camera_busy = true;
    assert(solar_os_rtspd_job.start(NULL, 3, both) == ESP_ERR_INVALID_STATE);
    camera_busy = false; audio_busy = true;
    assert(solar_os_rtspd_job.start(NULL, 3, both) == ESP_ERR_INVALID_STATE);
    audio_busy = false;
    assert(!live_tasks && camera_claims == camera_releases);
#else
    assert(solar_os_rtspd_job.start(NULL, 0, NULL) == ESP_ERR_NOT_SUPPORTED);
    assert(camera_claims == 0);
#endif
    assert(!live_allocations && !rtspd.session);
    assert(solar_os_rtspd_job.start(NULL, 3, audio) == ESP_OK);
    force_wait_timeout = true;
    solar_os_rtspd_job.stop(NULL);
    /* Pending cancellation retains all storage until both workers are joined. */
    assert(rtspd.session && live_allocations == 2U && live_tasks == 2U);
    assert(solar_os_rtspd_job.start(NULL, 3, audio) == ESP_ERR_INVALID_STATE);
    force_wait_timeout = false;
    solar_os_rtspd_job.stop(NULL);
    assert(!rtspd.session && !live_allocations && !live_bytes && !live_tasks);
    solar_os_rtspd_job.stop(NULL); /* Cleanup is idempotent. */
}
int main(void)
{
    assert(!rtspd.session && !rtspd.sources[0].scratch && !rtspd.sources[1].scratch);
    assert(!allocation_calls && !live_allocations);
    make_jpeg(); options_test(); failure_tests(); run_sessions(false);
#if SOLAR_OS_PACKAGE_SERVICE_CAMERA
    run_sessions(true);
#endif
    /* All diagnostic counters at maximum: shell printf must not truncate. */
    rtspd.clients = rtspd.rejected_clients = rtspd.frames = UINT32_MAX;
    rtspd.audio_blocks = rtspd.dropped_frames = rtspd.rtp_packets = UINT32_MAX;
    rtspd.rtcp_reports = rtspd.capture_errors = rtspd.jpeg_errors = UINT32_MAX;
    rtspd.send_errors = UINT32_MAX; rtspd.rtp_octets = UINT64_MAX;
    rtspd.options.port = UINT16_MAX; rtspd.options.fps = 30;
    rtspd.options.video = true; rtspd.audio_format.sample_rate = 192000;
    rtspd.audio_format.channels = 8;
    memset(rtspd.options.audio, 'a', sizeof(rtspd.options.audio) - 1);
    rtspd.options.audio[sizeof(rtspd.options.audio) - 1] = '\0';
    solar_os_context_t context = {0}; rtspd_job_detail(&context);
    assert(!latency_leases);
    puts("RTSP publisher source, RTP, reconnect and lifecycle tests: OK");
    return 0;
}
