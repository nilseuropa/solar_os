#define main original_media_suite
#include "script_media_test.c"
#undef main
#include "solar_os_native_media.h"
#include "solar_os_native_media_abi.h"

void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t kind, const char *tag)
{
    assert(!strcmp(tag, "native.buffer"));
    return solar_os_memory_calloc(1, bytes, kind, "script.media");
}

int main(void)
{
    const solar_os_native_media_api_v1_t *api = solar_os_native_media_get_service("media.video", 1, sizeof(*api));
    assert(api && !solar_os_native_media_get_service("image", 1, 0));
    const solar_os_camera_backend_ops_t ops = {.start = start, .stop = stop, .capture = capture, .release = release};
    const solar_os_camera_backend_t backend = {.driver = "fake", .ops = &ops};
    assert(!solar_os_camera_register_backend(&backend));
    assert(!solar_os_camera_stream_register("camera0"));
    const solar_os_stream_driver_t scalar = {.info = {.id = "sensor0", .provider = "fake", .format = "f32",
        .type = SOLAR_OS_STREAM_TYPE_SCALAR, .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE}, .read_scalar = read_scalar};
    assert(!solar_os_stream_register(&scalar));
    solar_os_native_media_t *s, *other;
    assert(!api->create("native", cancel, &cancel_requested, &s));
    assert(!api->create("other", NULL, NULL, &other));
    uint32_t source, frame;
    assert(api->open(s, "sensor0", NULL, &source) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !source && !starts);
    assert(api->snapshot(s, "sensor0", 320, 240, 12, &frame) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !frame && !starts);
    solar_os_native_video_options_v1_t options = {.width = 320, .height = 240, .jpeg_quality = 12, .timeout_ms = 1000};
    assert(!api->open(s, "camera0", &options, &source));
    assert(!api->acquire(s, source, &frame));
    solar_os_native_video_frame_v1_t view;
    assert(!api->frame(s, frame, &view) && view.data == jpeg && view.timestamp_us == 123456789012ULL && !view.network);
    assert(api->frame(other, frame, &view) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !view.data);
    assert(!api->release_frame(s, frame));
    assert(!api->acquire(s, source, &frame));
    fail_stop = true;
    assert(api->destroy(s) == SOLAR_OS_NATIVE_ERROR_FAILED && allocations == 2);
    fail_stop = false;
    assert(!api->destroy(s)); assert(starts == stops && releases == 2);
    assert(api->frame(other, frame, &view) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    assert(api->capabilities & SOLAR_OS_NATIVE_MEDIA_RTSP);
    assert(!api->rtsp_open(other, "rtsp://test/media", &source));
    assert(!api->rtsp_read(other, source, 10, &frame) && !frame);
    atomic_store(&client->pending, true);
    assert(!api->rtsp_read(other, source, 100, &frame) && frame);
    assert(!api->frame(other, frame, &view) && view.network && view.rtp_timestamp == 0xf1234567U);
    assert(!api->destroy(other)); assert(!workers && !clients && !allocations);
#else
    assert(!(api->capabilities & SOLAR_OS_NATIVE_MEDIA_RTSP));
    assert(api->rtsp_open(other, "rtsp://test", &source) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !source);
    assert(!api->destroy(other)); assert(!allocations);
#endif
    puts("native video table: source types, leases, stale IDs, camera/RTSP teardown passed");
    return 0;
}
