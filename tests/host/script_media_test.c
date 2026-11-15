#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "solar_os_script_media.h"
#include "solar_os_camera.h"
#include "solar_os_camera_stream.h"
#include "solar_os_memory.h"
#include "solar_os_log.h"
#include <time.h>
#include "freertos/task.h"
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
#include <pthread.h>
#include <stdatomic.h>
#include "solar_os_task.h"
#endif

static unsigned allocations;
static bool fail_alloc, fail_start, fail_capture, fail_stop, cancel_requested;
static unsigned starts, stops, releases;
static uint16_t camera_width, camera_height;
static const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
const char *esp_err_to_name(esp_err_t err) { (void)err; return "test error"; }

void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t kind, const char *tag)
{
    assert(kind == SOLAR_OS_MEMORY_EXTERNAL_REQUIRED && !strcmp(tag, "script.media"));
    if (fail_alloc) return NULL;
    void *p = calloc(n, size); if (p) ++allocations; return p;
}
void solar_os_memory_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{ (void)level; (void)tag; (void)fmt; assert(false); return ESP_OK; }

size_t strlcpy(char *dst, const char *src, size_t capacity)
{
    const size_t n = strlen(src);
    if (capacity) { size_t copy = n < capacity - 1 ? n : capacity - 1; memcpy(dst, src, copy); dst[copy] = 0; }
    return n;
}
uint64_t solar_os_time_uptime_ms(void) { return 42; }
esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *ms) { *ms = 4242; return ESP_OK; }
static bool cancel(void *user) { assert(user == &cancel_requested); return cancel_requested; }

static esp_err_t start(void *user, const solar_os_camera_config_t *config,
                        solar_os_camera_orientation_t orientation, solar_os_camera_sensor_info_t *sensor)
{
    (void)user; (void)orientation; assert(config->width && config->height);
    camera_width = config->width; camera_height = config->height;
    if (fail_start) return ESP_FAIL;
    ++starts; strcpy(sensor->name, "fake"); return ESP_OK;
}
static esp_err_t stop(void *user) { (void)user; if (fail_stop) return ESP_FAIL; ++stops; return ESP_OK; }
static esp_err_t capture(void *user, solar_os_camera_backend_frame_t *frame)
{
    (void)user; if (fail_capture) return ESP_ERR_TIMEOUT;
    *frame = (solar_os_camera_backend_frame_t){.data = jpeg, .length = sizeof(jpeg),
        .width = camera_width, .height = camera_height, .timestamp_us = 123456789012ULL, .release_token = &releases};
    return ESP_OK;
}
static void release(void *user, void *token) { (void)user; assert(token == &releases); ++releases; }
static esp_err_t read_scalar(void *user, const solar_os_stream_read_options_t *options, float *value)
{ (void)user; (void)options; *value = 3.25f; return ESP_OK; }
static esp_err_t read_bytes(void *user, solar_os_stream_handle_t *h, void *data,
    size_t size, uint32_t timeout, size_t *length)
{ (void)user; (void)h; (void)timeout; memset(data, 'x', size); *length = size; return ESP_OK; }
static esp_err_t write_bytes(void *user, solar_os_stream_handle_t *h, const void *data,
    size_t size, uint32_t timeout, size_t *length)
{ (void)user; (void)h; (void)data; (void)timeout; *length = size; return ESP_OK; }

int64_t esp_timer_get_time(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
void vTaskDelay(TickType_t ticks) { usleep(ticks * 1000); }
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
struct solar_os_rtsp_client { atomic_bool cancel, pending; bool video, audio; };
struct test_task { pthread_t thread; TaskFunction_t fn; void *arg; };
static solar_os_rtsp_client_t *client;
static bool fail_worker, fail_client;
static unsigned workers, clients;
void vTaskSuspend(TaskHandle_t task) { assert(!task); pthread_exit(NULL); }
static void *task_entry(void *arg) { struct test_task *t = arg; t->fn(t->arg); return NULL; }
BaseType_t solar_os_task_create_pinned_external(TaskFunction_t fn, const char *name, uint32_t stack,
    void *arg, UBaseType_t priority, TaskHandle_t *out, BaseType_t core, solar_os_task_role_t role)
{
    (void)priority; (void)core; assert(role == SOLAR_OS_TASK_ROLE_FOREGROUND);
    assert(stack == 8192 && !strcmp(name, "script-rtsp"));
    if (fail_worker) return 0;
    struct test_task *t = calloc(1, sizeof(*t)); assert(t); t->fn = fn; t->arg = arg;
    assert(!pthread_create(&t->thread, NULL, task_entry, t)); *out = t; ++workers; return pdPASS;
}
bool solar_os_task_wait_done(TaskHandle_t task, volatile bool *done, uint32_t timeout)
{
    (void)timeout; if (!task) return true;
    while (!__atomic_load_n(done, __ATOMIC_ACQUIRE)) usleep(1000);
    return true;
}
void solar_os_task_delete_external(TaskHandle_t task)
{ assert(!pthread_join(task->thread, NULL)); free(task); --workers; }
esp_err_t solar_os_rtsp_client_create(const char *url, const solar_os_rtsp_client_options_t *o,
    solar_os_rtsp_client_t **out)
{
    if (fail_client) return ESP_ERR_NO_MEM;
    if (strcmp(url, "rtsp://test/media") || (!o->audio && !o->video)) return ESP_ERR_INVALID_ARG;
    client = calloc(1, sizeof(*client)); assert(client); ++clients;
    client->video = o->video; client->audio = o->audio; *out = client; return ESP_OK;
}
esp_err_t solar_os_rtsp_client_run(solar_os_rtsp_client_t *c)
{ while (!atomic_load(&c->cancel)) usleep(1000); return ESP_OK; }
void solar_os_rtsp_client_cancel(solar_os_rtsp_client_t *c) { atomic_store(&c->cancel, true); }
void solar_os_rtsp_client_status(solar_os_rtsp_client_t *c, solar_os_rtsp_client_status_t *s)
{ *s = (solar_os_rtsp_client_status_t){.video = c->video, .audio = c->audio, .negotiated = true}; }
bool solar_os_rtsp_client_take_video(solar_os_rtsp_client_t *c, solar_os_rtp_jpeg_frame_t *f, uint64_t *arrival)
{
    if (!atomic_exchange(&c->pending, false)) return false;
    *f = (solar_os_rtp_jpeg_frame_t){.data = jpeg, .length = sizeof(jpeg), .width = 160,
        .height = 120, .timestamp = 0xf1234567U}; *arrival = 123456789012ULL; return true;
}
void solar_os_rtsp_client_release_video(solar_os_rtsp_client_t *c) { (void)c; ++releases; }
int64_t solar_os_rtsp_client_video_lateness(solar_os_rtsp_client_t *c, uint32_t timestamp, uint64_t arrival)
{ (void)c; assert(timestamp == 0xf1234567U && arrival == 123456789012ULL); return -25000; }
esp_err_t solar_os_rtsp_client_destroy(solar_os_rtsp_client_t *c) { free(c); --clients; return ESP_OK; }
#endif

int main(void)
{
    const solar_os_camera_backend_ops_t ops = {.start = start, .stop = stop, .capture = capture, .release = release};
    const solar_os_camera_backend_t backend = {.driver = "fake", .ops = &ops};
    assert(solar_os_camera_register_backend(&backend) == ESP_OK);
    assert(solar_os_camera_stream_register("camera0") == ESP_OK);
    const solar_os_stream_driver_t scalar = {.info = {.id = "sensor0", .provider = "fake", .format = "f32", .type = SOLAR_OS_STREAM_TYPE_SCALAR,
        .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE}, .read_scalar = read_scalar};
    const solar_os_stream_driver_t bytes = {.info = {.id = "bytes0", .provider = "fake", .format = "raw", .type = SOLAR_OS_STREAM_TYPE_BYTES,
        .direction = SOLAR_OS_STREAM_DIRECTION_DUPLEX}, .read = read_bytes, .write = write_bytes};
    assert(solar_os_stream_register(&scalar) == ESP_OK);
    assert(solar_os_stream_register(&bytes) == ESP_OK);
    solar_os_script_media_t *s, *other;
    fail_alloc = true;
    assert(solar_os_script_media_create("test", cancel, &cancel_requested, &s) == ESP_ERR_NO_MEM);
    fail_alloc = false;
    assert(solar_os_script_media_create("test", cancel, &cancel_requested, &s) == ESP_OK);
    assert(solar_os_script_media_create("other", cancel, &cancel_requested, &other) == ESP_OK);
    uint32_t frame, handle;
    solar_os_script_media_frame_t info;
    fail_start = true;
    assert(solar_os_script_media_snapshot(s, "camera0", 320, 240, 12, &frame) == ESP_FAIL);
    fail_start = false; fail_capture = true;
    assert(solar_os_script_media_snapshot(s, "camera0", 320, 240, 12, &frame) == ESP_ERR_TIMEOUT);
    fail_capture = false;
    assert(solar_os_script_media_snapshot(s, "camera0", 320, 240, 64, &frame) == ESP_ERR_INVALID_ARG);
    assert(solar_os_script_media_snapshot(s, "camera0", 320, 240, 12, &frame) == ESP_OK);
    assert(solar_os_script_media_frame(s, frame, &info) == ESP_OK);
    assert(info.jpeg.data == jpeg && info.jpeg.timestamp_us == 123456789012ULL && !info.network);
    assert(solar_os_script_media_frame(other, frame, &info) == ESP_ERR_INVALID_ARG);
    uint32_t ignored;
    assert(solar_os_script_media_snapshot(other, "camera0", 320, 240, 12, &ignored) == ESP_ERR_INVALID_STATE);
    int file_errno = EACCES;
    assert(solar_os_script_media_save(s, frame, "/nonexistent-parent/script-media.jpg", &file_errno) == ESP_FAIL);
    assert(file_errno == ENOENT);
    assert(solar_os_script_media_frame(s, frame, &info) == ESP_OK); /* save does not consume a frame */
    assert(solar_os_script_media_save(s, frame, "/tmp", &file_errno) == ESP_FAIL && file_errno == EISDIR);
    /* Buffered fwrite can succeed; fclose must still report a full device. */
    assert(solar_os_script_media_save(s, frame, "/dev/full", &file_errno) == ESP_FAIL && file_errno == ENOSPC);
    assert(solar_os_script_media_save(s, 0, "/tmp", &file_errno) == ESP_ERR_INVALID_ARG && !file_errno);
    assert(solar_os_script_media_save(s, frame, "", &file_errno) == ESP_ERR_INVALID_ARG && !file_errno);
    char saved_path[] = "/tmp/solar-script-media-save-XXXXXX";
    int saved_fd = mkstemp(saved_path); assert(saved_fd >= 0); close(saved_fd);
    assert(solar_os_script_media_save(s, frame, saved_path, &file_errno) == ESP_OK && !file_errno);
    FILE *saved = fopen(saved_path, "rb"); assert(saved);
    uint8_t saved_bytes[sizeof(jpeg)];
    assert(fread(saved_bytes, 1, sizeof(saved_bytes), saved) == sizeof(saved_bytes));
    assert(!memcmp(saved_bytes, jpeg, sizeof(jpeg)) && fgetc(saved) == EOF);
    assert(!fclose(saved) && !unlink(saved_path));
    fail_stop = true;
    assert(solar_os_script_media_release(s, frame) == ESP_FAIL);
    assert(solar_os_script_media_frame(s, frame, &info) == ESP_ERR_INVALID_STATE);
    fail_stop = false;
    assert(solar_os_script_media_release(s, frame) == ESP_OK);
    assert(solar_os_script_media_frame(s, frame, &info) == ESP_ERR_INVALID_ARG);
    assert(solar_os_script_media_release(s, frame) == ESP_ERR_INVALID_ARG);

    const solar_os_stream_open_options_t video_options = {.direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
        .requested_video = {.codec = SOLAR_OS_STREAM_VIDEO_JPEG, .width = 640, .height = 480, .jpeg_quality = 20}};
    assert(solar_os_script_media_open(s, "camera0", &video_options, &handle) == ESP_OK);
    solar_os_stream_info_t stream_info;
    assert(solar_os_script_media_stream_info(s, handle, &stream_info) == ESP_OK);
    assert(stream_info.video.width == 640 && stream_info.video.height == 480 && stream_info.video.jpeg_quality == 20);
    assert(solar_os_script_media_acquire(s, handle, &frame) == ESP_OK);
    assert(solar_os_script_media_acquire(s, handle, &ignored) == ESP_ERR_INVALID_STATE);
    assert(solar_os_script_media_release(s, frame) == ESP_OK);
    assert(solar_os_script_media_acquire(s, handle, &ignored) == ESP_OK && ignored != frame);
    assert(solar_os_script_media_close(s, handle) == ESP_OK); /* closes a held frame */
    assert(solar_os_script_media_frame(s, ignored, &info) == ESP_ERR_INVALID_ARG);
    assert(solar_os_script_media_close(s, handle) == ESP_ERR_INVALID_ARG);
    solar_os_stream_open_options_t options = {.direction = SOLAR_OS_STREAM_DIRECTION_SOURCE};
    assert(solar_os_script_media_open(s, "sensor0", &options, &handle) == ESP_OK);
    float value;
    const solar_os_stream_read_options_t read_options = {.timeout_ms = 50};
    assert(solar_os_script_media_scalar(s, handle, &read_options, &value) == ESP_OK && value == 3.25f);
    assert(solar_os_script_media_acquire(s, handle, &ignored) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_script_media_close_all(s) == ESP_OK);
    uint32_t handles[SOLAR_OS_SCRIPT_MEDIA_STREAMS];
    options.direction = SOLAR_OS_STREAM_DIRECTION_DUPLEX;
    for (size_t i = 0; i < SOLAR_OS_SCRIPT_MEDIA_STREAMS; ++i)
        assert(solar_os_script_media_open(s, "bytes0", &options, &handles[i]) == ESP_OK);
    assert(solar_os_script_media_open(s, "bytes0", &options, &handle) == ESP_ERR_NO_MEM);
    char buffer[32]; size_t length;
    assert(solar_os_script_media_read(s, handles[0], buffer, sizeof(buffer), 0, &length) == ESP_OK && length == sizeof(buffer));
    assert(solar_os_script_media_write(s, handles[0], buffer, sizeof(buffer), 0, &length) == ESP_OK);
    assert(solar_os_script_media_read(s, handles[0], buffer, 16385, 0, &length) == ESP_ERR_INVALID_ARG);
    cancel_requested = true;
    assert(solar_os_script_media_read(s, handles[0], buffer, sizeof(buffer), 0, &length) == ESP_ERR_TIMEOUT);
    cancel_requested = false;
    assert(solar_os_script_media_close_all(s) == ESP_OK);

#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    fail_client = true;
    assert(solar_os_script_media_rtsp_open(s, "rtsp://test/media", true, true, &handle) == ESP_ERR_NO_MEM);
    fail_client = false; fail_worker = true;
    assert(solar_os_script_media_rtsp_open(s, "rtsp://test/media", true, true, &handle) == ESP_ERR_NO_MEM);
    assert(!clients && !workers);
    fail_worker = false;
    assert(solar_os_script_media_rtsp_open(s, "rtsp://test/media", true, false, &handle) == ESP_OK);
    assert(solar_os_script_media_rtsp_open(s, "rtsp://test/media", true, false, &ignored) == ESP_ERR_INVALID_STATE);
    assert(solar_os_script_media_rtsp_read(other, handle, 0, &ignored) == ESP_ERR_INVALID_ARG);
    assert(solar_os_script_media_rtsp_read(s, handle, 10, &frame) == ESP_OK && !frame);
    atomic_store(&client->pending, true);
    assert(solar_os_script_media_rtsp_read(s, handle, 0, &frame) == ESP_OK && frame);
    assert(solar_os_script_media_frame(s, frame, &info) == ESP_OK);
    assert(info.network && info.rtp_timestamp == 0xf1234567U);
    int64_t lateness;
    assert(solar_os_script_media_rtsp_lateness(s, handle, frame, &lateness) == ESP_OK && lateness == -25000);
    assert(solar_os_script_media_rtsp_read(s, handle, 0, &ignored) == ESP_ERR_INVALID_STATE);
    assert(solar_os_script_media_release(s, frame) == ESP_OK);
    atomic_store(&client->pending, true);
    assert(solar_os_script_media_rtsp_read(s, handle, 0, &frame) == ESP_OK && frame);
    assert(solar_os_script_media_close(s, handle) == ESP_OK);
    assert(!clients && !workers && solar_os_script_media_frame(s, frame, &info) == ESP_ERR_INVALID_ARG);
    assert(solar_os_script_media_rtsp_open(s, "rtsp://test/media", false, true, &handle) == ESP_OK);
    solar_os_rtsp_client_status_t status; bool ended;
    assert(solar_os_script_media_rtsp_status(s, handle, &status, &ended) == ESP_OK);
    assert(status.audio && !status.video && !ended);
    cancel_requested = true;
    assert(solar_os_script_media_rtsp_read(s, handle, 1000, &frame) == ESP_ERR_TIMEOUT);
    cancel_requested = false;
#endif
    assert(solar_os_script_media_snapshot(s, "camera0", 320, 240, 12, &frame) == ESP_OK);
    solar_os_script_media_destroy(s); solar_os_script_media_destroy(other);
    assert(!allocations && starts == stops);
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    assert(!clients && !workers);
#endif
    assert(solar_os_camera_stream_unregister("camera0") == ESP_OK);
    assert(solar_os_camera_unregister_backend("fake") == ESP_OK);
    puts("script media lease/lifecycle tests passed");
    return 0;
}
