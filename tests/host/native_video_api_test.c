#define main original_media_suite
#include "script_media_test.c"
#undef main
#include "solar_os_native_media.h"
#include "solar_os_native_media_abi.h"
#include "solar_os_native_rtsp_auth.h"
#include "solar_os_native_rtsp_client.h"

/* This fixture supplies a synthetic RTSP source for media.video.
 * The loopback rtsp_client_test exercises the standalone provider. */
const void *solar_os_native_rtsp_client_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}

void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t kind, const char *tag)
{
    assert(!strcmp(tag, "native.buffer"));
    return solar_os_memory_calloc(1, bytes, kind, "script.media");
}

const void *solar_os_native_rtsp_auth_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
    return solar_os_native_rtsp_client_get_service(name, abi, minimum);
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
    char photo_path[] = "/tmp/solaros-native-video-test-XXXXXX";
    int photo = mkstemp(photo_path);
    assert(photo >= 0 && close(photo) == 0);
    int file_error = -1;
    assert(!api->save_frame(s, frame, photo_path, &file_error) && !file_error);
    FILE *saved = fopen(photo_path, "rb");
    uint8_t saved_jpeg[sizeof(jpeg)];
    assert(saved && fread(saved_jpeg, 1, sizeof(saved_jpeg), saved) == sizeof(jpeg));
    assert(!memcmp(saved_jpeg, jpeg, sizeof(jpeg)) && fclose(saved) == 0);
    assert(unlink(photo_path) == 0);
    assert(!api->release_frame(s, frame));
    assert(!api->acquire(s, source, &frame));
    fail_stop = true;
    assert(api->reset(s) == SOLAR_OS_NATIVE_ERROR_FAILED && allocations == 2);
    fail_stop = false;
    assert(!api->reset(s) && !api->destroy(s)); assert(starts == stops && releases == 2);
    assert(api->frame(other, frame, &view) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    assert(api->capabilities & SOLAR_OS_NATIVE_MEDIA_RTSP);
    char url[128], label[128];
    assert(!api->rtsp_normalize_url("guest:example@test/media", url, sizeof(url)));
    assert(!strcmp(url, "rtsp://guest:example@test/media"));
    assert(!api->rtsp_redact_url(url, label, sizeof(label)));
    assert(!strcmp(label, "guest@test/media"));
    assert(api->rtsp_open_tracks(other, url, false, false, &source) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->rtsp_open(other, "rtsp://test/media", &source));
    assert(!api->rtsp_read(other, source, 10, &frame) && !frame);
    atomic_store(&client->pending, true);
    assert(!api->rtsp_read(other, source, 100, &frame) && frame);
    assert(!api->frame(other, frame, &view) && view.network && view.rtp_timestamp == 0xf1234567U);
    int64_t lateness;
    assert(!api->rtsp_lateness(other, source, frame, &lateness) && lateness == -25000);
    solar_os_rtsp_client_status_t status;
    bool ended;
    assert(!api->rtsp_status(other, source, &status, &ended) && status.video && !status.audio && !ended);
    assert(!api->reset(other) && !workers && !clients);
    assert(!api->rtsp_open_tracks(other, "rtsp://test/media", true, true, &source));
    assert(!api->rtsp_status(other, source, &status, &ended) && status.video && status.audio);
    assert(!api->destroy(other)); assert(!workers && !clients && !allocations);
#else
    assert(!(api->capabilities & SOLAR_OS_NATIVE_MEDIA_RTSP));
    assert(api->rtsp_open(other, "rtsp://test", &source) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !source);
    char unavailable[8] = "test";
    assert(api->rtsp_normalize_url("test", unavailable, sizeof(unavailable)) ==
           SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !unavailable[0]);
    assert(!api->destroy(other)); assert(!allocations);
#endif
    puts("native video table: source types, leases, stale IDs, camera/RTSP teardown passed");
    return 0;
}
