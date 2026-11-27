#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_client.h"
#include "solar_os_native_stream.h"
#include "solar_os_native_stream_provider.h"
#include "solar_os_native_stream_provider_abi.h"
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
typedef struct {
    const solar_os_native_stream_provider_api_v1_t *api;
    solar_os_native_handle_t handle;
    unsigned opens, closes;
    bool fail_close, bad_size;
} stream_provider_fixture_t;

static unsigned provider_leases;

int solar_os_native_library_retain(uint32_t lease, uint32_t *retained)
{
    if (lease != 6) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *retained = 7;
    provider_leases++;
    return SOLAR_OS_NATIVE_OK;
}

int solar_os_native_library_release(uint32_t retained)
{
    assert(retained == 7 && provider_leases == 1);
    provider_leases--;
    return SOLAR_OS_NATIVE_OK;
}

static int provider_negotiate(void *user, const solar_os_native_stream_options_v1_t *request,
                              solar_os_native_stream_format_v1_t *selected)
{
    (void)user;
    assert(request->direction == SOLAR_OS_NATIVE_STREAM_SOURCE);
    selected->sample_rate = request->format.sample_rate;
    return SOLAR_OS_NATIVE_OK;
}

static int provider_open(void *user, const char *owner, const solar_os_native_stream_options_v1_t *request,
                          const solar_os_native_stream_format_v1_t *selected, void **session)
{
    stream_provider_fixture_t *fixture = user;
    assert(provider_leases == 1 && !strcmp(owner, "host"));
    assert(selected->sample_rate == 22050 && request->format.sample_rate == 22050);
    *session = &fixture->opens;
    fixture->opens++;
    return SOLAR_OS_NATIVE_OK;
}

static int provider_close(void *user, void *session)
{
    stream_provider_fixture_t *fixture = user;
    assert(provider_leases == 1 && session == &fixture->opens);
    if (fixture->fail_close) return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    fixture->closes++;
    return SOLAR_OS_NATIVE_OK;
}

static int provider_read(void *user, void *session, void *data, size_t capacity, uint32_t timeout, size_t *size)
{
    stream_provider_fixture_t *fixture = user;
    assert(provider_leases == 1 && session == &fixture->opens);
    assert(capacity >= 2 && timeout == 35);
    uint8_t *bytes = data;
    bytes[0] = 42;
    bytes[1] = 3;
    *size = fixture->bad_size ? capacity + 1 : 2;
    return SOLAR_OS_NATIVE_OK;
}

static int provider_observe(void *user, const solar_os_native_stream_read_options_v1_t *request,
                            solar_os_native_stream_sample_v1_t *value)
{
    stream_provider_fixture_t *fixture = user;
    assert(provider_leases == 1 && request->window_ms == 17);
    assert(fixture->api->provider_close(fixture->handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
    *value = (solar_os_native_stream_sample_v1_t){3.5f, 12000};
    return SOLAR_OS_NATIVE_OK;
}

static void test_stream_provider(const solar_os_native_stream_api_v1_t *streams)
{
    const solar_os_native_stream_provider_api_v1_t *api = solar_os_native_stream_provider_get_service(
        SOLAR_OS_NATIVE_STREAM_PROVIDER_SERVICE, SOLAR_OS_NATIVE_STREAM_PROVIDER_ABI, sizeof(*api));
    assert(api && !solar_os_native_stream_provider_get_service("stream.provider", 2, 8));
    stream_provider_fixture_t fixture = {.api = api};
    solar_os_native_stream_provider_endpoint_v1_t endpoints[] = {
        {
            .info = {.id = "provider.audio", .provider = "native", .format = "s16le",
                .type = SOLAR_OS_NATIVE_STREAM_AUDIO, .direction = SOLAR_OS_NATIVE_STREAM_SOURCE,
                .sharing = SOLAR_OS_NATIVE_STREAM_EXCLUSIVE,
                .media = {.sample_rate = 44100, .channels = 1, .bits_per_sample = 16}},
            .callbacks = {.user = &fixture, .negotiate = provider_negotiate, .open = provider_open,
                .close = provider_close, .read = provider_read},
        },
        {
            .info = {.id = "provider.scalar", .provider = "native", .format = "f32",
                .type = SOLAR_OS_NATIVE_STREAM_SCALAR, .direction = SOLAR_OS_NATIVE_STREAM_SOURCE,
                .sharing = SOLAR_OS_NATIVE_STREAM_SHARED},
            .callbacks = {.user = &fixture, .observe_sample = provider_observe, .read_sample = provider_observe},
        },
    };
    const solar_os_stream_driver_t replacement = {
        .info = {.id = "provider.scalar", .provider = "replacement", .format = "f32",
            .type = SOLAR_OS_STREAM_TYPE_SCALAR, .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE},
        .read_scalar_sample = sample,
    };
    assert(solar_os_stream_register(&replacement) == ESP_OK);
    assert(api->provider_register(6, endpoints, 2, &fixture.handle) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!fixture.handle && !provider_leases && !allocations && streams->count() == 1);
    assert(solar_os_stream_unregister("provider.scalar") == ESP_OK);

    assert(api->provider_register(6, endpoints, 2, &fixture.handle) == SOLAR_OS_NATIVE_OK);
    assert(fixture.handle && provider_leases == 1 && streams->count() == 2);
    endpoints[0].info.id[0] = 'X';
    solar_os_native_stream_info_v1_t info;
    assert(streams->find("provider.audio", &info) == SOLAR_OS_NATIVE_OK && info.media.sample_rate == 44100);
    solar_os_native_stream_sample_v1_t value;
    const solar_os_native_stream_read_options_v1_t sample_options = {17, 20};
    assert(streams->observe_sample("provider.scalar", &sample_options, &value) == SOLAR_OS_NATIVE_OK);
    assert(value.value == 3.5f && value.uptime_ms == 12000);

    solar_os_native_handle_t handle;
    assert(streams->open("provider.audio", "host", NULL, &handle) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!handle && !fixture.opens);
    const solar_os_native_stream_options_v1_t request = {
        .direction = SOLAR_OS_NATIVE_STREAM_SOURCE, .format = {.sample_rate = 22050}};
    assert(streams->open("provider.audio", "host", &request, &handle) == SOLAR_OS_NATIVE_OK);
    assert(handle && fixture.opens == 1);
    assert(streams->handle_info(handle, &info) == SOLAR_OS_NATIVE_OK);
    assert(info.direction == SOLAR_OS_NATIVE_STREAM_SOURCE && info.media.sample_rate == 22050);
    assert(!strcmp(streams->type_name(info.type), "audio"));
    assert(!strcmp(streams->direction_name(info.direction), "source"));
    assert(!strcmp(streams->sharing_name(info.sharing), "exclusive"));
    assert(!strcmp(streams->audio_format_name(info.media.audio_format), "s16le"));
    uint8_t bytes[2];
    size_t read = 0;
    assert(streams->read(handle, bytes, sizeof(bytes), 35, &read) == SOLAR_OS_NATIVE_OK);
    assert(read == 2 && bytes[0] == 42 && bytes[1] == 3);
    fixture.bad_size = true;
    assert(streams->read(handle, bytes, sizeof(bytes), 35, &read) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && !read);
    assert(api->provider_close(fixture.handle) == SOLAR_OS_NATIVE_ERROR_BUSY && streams->count() == 2);
    fixture.fail_close = true;
    assert(streams->close(handle) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && streams->valid(handle));
    fixture.fail_close = false;
    assert(streams->close(handle) == SOLAR_OS_NATIVE_OK && fixture.closes == 1);
    assert(streams->handle_info(handle, &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !info.id[0]);

    assert(solar_os_stream_unregister("provider.scalar") == ESP_OK);
    assert(solar_os_stream_register(&replacement) == ESP_OK);
    assert(api->provider_close(fixture.handle) == SOLAR_OS_NATIVE_OK);
    assert(streams->find("provider.audio", &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(streams->find("provider.scalar", &info) == SOLAR_OS_NATIVE_OK && !strcmp(info.provider, "replacement"));
    assert(solar_os_stream_unregister("provider.scalar") == ESP_OK);
    assert(!provider_leases && !allocations && !solar_os_native_handle_count());
    puts("Stream provider ABI: atomic groups, negotiated PCM, observer pins and retryable close passed");
}

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
    test_stream_provider(api);
    assert(!allocations && !solar_os_native_handle_count()); puts("native streams with real registry: values, typed open and frame ownership passed"); return 0;
}
