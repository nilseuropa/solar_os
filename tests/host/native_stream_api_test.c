#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_client.h"
#include "solar_os_native_stream.h"
#include "solar_os_native_handles.h"
#include "solar_os_memory.h"
#include "solar_os_stream.h"
static unsigned allocations, opened, closed, acquired, released;
static bool fail_close, fail_release;
static const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
size_t strlcpy(char *out, const char *in, size_t capacity)
{ size_t size = strlen(in); if (capacity) { size_t n = size < capacity - 1 ? size : capacity - 1; memcpy(out, in, n); out[n] = 0; } return size; }
uint64_t solar_os_time_uptime_ms(void) { return 1234; }
esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *out) { *out = UINT64_C(5000000000); return ESP_OK; }
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{ assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag); void *p = calloc(n, size); if (p) ++allocations; return p; }
void solar_os_memory_free(void *p) { if (p) --allocations; free(p); }
static esp_err_t open_video(void *user, const char *owner, const solar_os_stream_open_options_t *options, solar_os_stream_handle_t *handle)
{
    assert(user == &opened && !strcmp(owner, "native") && handle->type == SOLAR_OS_STREAM_TYPE_VIDEO);
    assert(options->requested_video.width == 320 && options->requested_video.height == 240 && options->requested_video.jpeg_quality == 75);
    ++opened; return ESP_OK;
}
static esp_err_t close_checked(void *user, solar_os_stream_handle_t *handle)
{ assert(user == &opened && handle->type == SOLAR_OS_STREAM_TYPE_VIDEO); if (fail_close) return ESP_ERR_TIMEOUT; ++closed; return ESP_OK; }
static esp_err_t acquire(void *user, solar_os_stream_handle_t *handle, solar_os_stream_video_frame_t *out)
{
    assert(user == &opened && handle->type == SOLAR_OS_STREAM_TYPE_VIDEO); ++acquired;
    *out = (solar_os_stream_video_frame_t){jpeg, sizeof(jpeg), 320, 240, UINT64_C(6000000000)}; return ESP_OK;
}
static esp_err_t release(void *user, solar_os_stream_handle_t *handle, solar_os_stream_video_frame_t *frame)
{ assert(user == &opened && handle->leased_frame == frame && frame->data == jpeg); if (fail_release) return ESP_ERR_TIMEOUT; ++released; return ESP_OK; }
static esp_err_t sample(void *user, const solar_os_stream_read_options_t *options, solar_os_stream_scalar_sample_t *out)
{ (void)user; assert(options && options->window_ms == 17 && options->timeout_ms == 42); *out = (solar_os_stream_scalar_sample_t){1.25f, UINT64_C(7000000000)}; return ESP_OK; }
int main(void)
{
    const solar_os_native_stream_api_v1_t *api = solar_os_native_stream_get_service("stream", 1, sizeof(*api)); assert(api);
    solar_os_stream_driver_t video = {.info = {.id = "video", .provider = "camera", .device = "camera0", .format = "jpeg", .type = SOLAR_OS_STREAM_TYPE_VIDEO,
        .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE, .sharing = SOLAR_OS_STREAM_SHARING_EXCLUSIVE,
        .video = {SOLAR_OS_STREAM_VIDEO_JPEG, 640, 480, 80}}, .open = open_video, .close_checked = close_checked,
        .acquire_frame = acquire, .release_frame = release, .user = &opened};
    assert(!solar_os_stream_register(&video));
    solar_os_native_stream_info_v1_t info; assert(!api->find("video", &info));
    assert(info.type == SOLAR_OS_NATIVE_STREAM_VIDEO && info.media.width == 640 && info.media.height == 480 && !strcmp(info.provider, "camera"));
    solar_os_native_stream_options_v1_t options = {.direction = SOLAR_OS_NATIVE_STREAM_SOURCE,
        .format = {.width = 320, .height = 240, .jpeg_quality = 75}};
    solar_os_native_handle_t handle; options.format.width = 65536;
    assert(api->open("video", "native", &options, &handle) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !handle && !opened);
    options.format.width = 320; assert(!api->open("video", "native", &options, &handle) && api->valid(handle) && opened == 1);
    assert(solar_os_stream_unregister("video") == ESP_ERR_INVALID_STATE);
    solar_os_native_stream_frame_v1_t frame; assert(!api->frame_acquire(handle, &frame));
    assert(frame.data == jpeg && frame.size == sizeof(jpeg) && frame.timestamp_us == UINT64_C(6000000000) && frame.width == 320);
    assert(api->frame_acquire(handle, &frame) == SOLAR_OS_NATIVE_ERROR_BUSY && acquired == 1);
    assert(api->close(handle) == SOLAR_OS_NATIVE_ERROR_BUSY && !closed);
    fail_release = true; assert(api->frame_release(handle) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(api->close(handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
    fail_release = false; assert(!api->frame_release(handle) && released == 1);
    fail_close = true; assert(api->close(handle) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && api->valid(handle));
    fail_close = false; assert(!api->close(handle) && closed == 1 && !api->valid(handle)); assert(!solar_os_stream_unregister("video"));
    solar_os_stream_driver_t scalar = {.info = {.id = "scalar", .provider = "test", .format = "f32", .type = SOLAR_OS_STREAM_TYPE_SCALAR, .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
        .sharing = SOLAR_OS_STREAM_SHARING_SHARED},
        .read_scalar_sample = sample, .observe_scalar_sample = sample};
    assert(!solar_os_stream_register(&scalar)); assert(!api->open("scalar", "native", NULL, &handle));
    solar_os_native_stream_sample_v1_t value; solar_os_native_stream_read_options_v1_t read_options = {17, 42};
    assert(!api->read_sample(handle, &read_options, &value) && value.value == 1.25f && value.uptime_ms == UINT64_C(7000000000));
    assert(!api->observe_sample("scalar", &read_options, &value) && value.uptime_ms == UINT64_C(7000000000));
    assert(!api->close(handle));
    solar_os_stream_handle_t direct = SOLAR_OS_STREAM_HANDLE_INIT;
    assert(solar_os_stream_open_typed("scalar", "native", SOLAR_OS_STREAM_TYPE_VIDEO, NULL, &direct) == ESP_ERR_INVALID_STATE);
    assert(!solar_os_stream_handle_valid(&direct)); assert(!solar_os_stream_unregister("scalar"));
    assert(!allocations && !solar_os_native_handle_count()); puts("native streams with real registry: values, typed open and frame ownership passed"); return 0;
}
