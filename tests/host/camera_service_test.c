#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_camera.h"
#include "solar_os_native_camera.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_runtime.h"
#include "solar_os_native_support.h"
#include "solar_os_stream.h"
#include "solar_os_memory.h"

typedef struct {
    bool started;
    bool fail_capture;
    bool fail_stop;
    unsigned starts;
    unsigned stops;
    unsigned releases;
    solar_os_camera_config_t config;
    solar_os_camera_orientation_t orientation;
} fake_camera_t;

static const uint8_t fake_jpeg[] = {0xff, 0xd8, 0xff, 0xd9};

static esp_err_t fake_start(void *ctx,
                            const solar_os_camera_config_t *config,
                            solar_os_camera_orientation_t orientation,
                            solar_os_camera_sensor_info_t *sensor)
{
    fake_camera_t *fake = ctx;
    fake->started = true;
    fake->starts++;
    fake->config = *config;
    fake->orientation = orientation;
    sensor->product_id = 0x26U;
    strcpy(sensor->name, "OV2640");
    return ESP_OK;
}

static esp_err_t fake_stop(void *ctx)
{
    fake_camera_t *fake = ctx;
    if (fake->fail_stop) return ESP_ERR_TIMEOUT;
    fake->started = false;
    fake->stops++;
    return ESP_OK;
}

static esp_err_t fake_capture(void *ctx,
                              solar_os_camera_backend_frame_t *frame)
{
    fake_camera_t *fake = ctx;
    if (fake->fail_capture) {
        return ESP_ERR_TIMEOUT;
    }
    *frame = (solar_os_camera_backend_frame_t) {
        .data = fake_jpeg,
        .length = sizeof(fake_jpeg),
        .width = fake->config.width,
        .height = fake->config.height,
        .timestamp_us = 1234U,
        .release_token = fake,
    };
    return ESP_OK;
}

static void fake_release(void *ctx, void *release_token)
{
    fake_camera_t *fake = ctx;
    assert(release_token == fake);
    fake->releases++;
}

static esp_err_t fake_orientation(void *ctx, solar_os_camera_orientation_t orientation,
                                  solar_os_camera_orientation_t *applied)
{
    fake_camera_t *fake = ctx;
    fake->orientation = orientation;
    *applied = orientation;
    return ESP_OK;
}

static esp_err_t fake_resolutions(void *ctx, solar_os_camera_resolution_t *out,
                                   size_t capacity, size_t *count)
{
    (void)ctx;
    const solar_os_camera_resolution_t modes[] = {{320,240}, {416,416}, {640,480}};
    *count = 3;
    for (size_t i = 0; i < capacity && i < 3; i++) out[i] = modes[i];
    return ESP_OK;
}

static esp_err_t fake_control(void *ctx, const char *protocol, const void *request)
{
    assert(ctx && request && !strcmp(protocol, "test"));
    return *(const esp_err_t *)request;
}
static unsigned invalidations;
static void fake_invalidate(void *ctx) { assert(ctx); ++invalidations; }

static unsigned native_allocations;

void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag);
    void *memory = calloc(count, size);
    if (memory) ++native_allocations;
    return memory;
}

void solar_os_memory_free(void *memory)
{
    if (memory) --native_allocations;
    free(memory);
}

static void test_native_camera(const solar_os_camera_backend_ops_t *operations)
{
    const solar_os_native_camera_api_v1_t *camera = solar_os_native_camera_get_service(
        SOLAR_OS_NATIVE_CAMERA_SERVICE, SOLAR_OS_NATIVE_CAMERA_ABI, sizeof(*camera));
    assert(camera);
    fake_camera_t fake = {0};
    const solar_os_camera_backend_t backend = {.driver = "abi-camera", .ops = operations, .ctx = &fake};
    assert(solar_os_camera_register_backend(&backend) == ESP_OK);
    solar_os_camera_config_t config = camera->default_config();
    solar_os_native_handle_t owner = 0, competing = 0;
    assert(!camera->open("abi", &owner) && owner);
    assert(camera->open("competing", &competing) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !competing);
    assert(!camera->set_running(owner, true, &config));
    solar_os_camera_frame_t frame;
    assert(!camera->capture(owner, &frame));
    assert(frame.data == fake_jpeg && frame.length == sizeof(fake_jpeg) && frame.timestamp_us == 1234);
    solar_os_camera_frame_t busy;
    assert(camera->capture(owner, &busy) == SOLAR_OS_NATIVE_ERROR_BUSY && !busy.data);
    assert(camera->close(owner) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(camera->set_running(owner, false, NULL) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(camera->invalidate(owner) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(!camera->release_frame(owner) && fake.releases == 1);
    fake.fail_stop = true;
    assert(camera->close(owner) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    solar_os_camera_status_t status;
    assert(!camera->status(&status) && status.owner_leased && status.last_error == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    fake.fail_stop = false;
    assert(!camera->close(owner) && !native_allocations && !solar_os_native_handle_count());
    assert(camera->capture(owner, &frame) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(solar_os_camera_unregister_backend(backend.driver) == ESP_OK);
}

static unsigned provider_leases;

int solar_os_native_library_retain(uint32_t token, uint32_t *retained)
{
    assert(token == 10 && retained);
    ++provider_leases;
    *retained = 11;
    return SOLAR_OS_NATIVE_OK;
}

int solar_os_native_library_release(uint32_t token)
{
    assert(token == 11 && provider_leases);
    --provider_leases;
    return SOLAR_OS_NATIVE_OK;
}

static int portable_start(void *user, const solar_os_camera_config_t *config, solar_os_camera_orientation_t orientation,
                          solar_os_camera_sensor_info_t *sensor)
{ return solar_os_native_result(fake_start(user, config, orientation, sensor)); }
static int portable_stop(void *user)
{ return solar_os_native_result(fake_stop(user)); }
static int portable_capture(void *user, solar_os_camera_backend_frame_t *frame)
{ return solar_os_native_result(fake_capture(user, frame)); }
static esp_err_t other_scalar(void *user, const solar_os_stream_read_options_t *options, float *out)
{ (void)user; (void)options; *out = 1; return ESP_OK; }

static void test_native_camera_provider(void)
{
    const solar_os_native_camera_api_v1_t *camera = solar_os_native_camera_get_service(
        SOLAR_OS_NATIVE_CAMERA_SERVICE, SOLAR_OS_NATIVE_CAMERA_ABI, sizeof(*camera));
    assert(camera && camera->provider_register && camera->provider_close);
    fake_camera_t fake = {0};
    char name[] = "portable-camera";
    solar_os_camera_config_t config = {.width = 416, .height = 416, .jpeg_quality = 18};
    solar_os_native_camera_provider_v1_t descriptor = {
        .driver = name, .library_lease = 10, .stream_name = name, .default_config = &config,
        .user = &fake, .start = portable_start, .stop = portable_stop,
        .capture = portable_capture, .release = fake_release,
    };
    static const solar_os_stream_driver_t foreign = {
        .info = {.id = "portable-camera", .provider = "foreign", .device = "other", .format = "f32",
                 .type = SOLAR_OS_STREAM_TYPE_SCALAR, .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE},
        .read_scalar = other_scalar,
    };
    assert(solar_os_stream_register(&foreign) == ESP_OK);
    solar_os_native_handle_t provider;
    assert(camera->provider_register(&descriptor, &provider) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!provider && !provider_leases && !native_allocations);
    solar_os_camera_status_t status;
    assert(!camera->status(&status) && !status.backend_registered);
    assert(solar_os_stream_unregister(foreign.info.id) == ESP_OK);

    assert(!camera->provider_register(&descriptor, &provider) && provider && provider_leases == 1);
    memset(&descriptor, 0, sizeof(descriptor));
    name[0] = 0;
    config.width = 1;
    solar_os_stream_info_t info;
    assert(solar_os_stream_get_info(foreign.info.id, &info) == ESP_OK && info.type == SOLAR_OS_STREAM_TYPE_VIDEO);
    assert(info.video.width == 416 && info.video.jpeg_quality == 18);
    solar_os_stream_handle_t stream = SOLAR_OS_STREAM_HANDLE_INIT;
    solar_os_stream_open_options_t options = {.direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
        .requested_video = {.codec = SOLAR_OS_STREAM_VIDEO_JPEG}};
    assert(solar_os_stream_open_ex(foreign.info.id, "consumer", &options, &stream) == ESP_OK);
    solar_os_stream_video_frame_t frame;
    assert(solar_os_stream_acquire_frame(&stream, &frame) == ESP_OK && frame.data == fake_jpeg && frame.width == 416);
    assert(camera->provider_close(provider) == SOLAR_OS_NATIVE_ERROR_BUSY && provider_leases == 1);
    assert(solar_os_stream_release_frame(&stream, &frame) == ESP_OK);
    assert(solar_os_stream_close_ex(&stream) == ESP_OK && fake.releases == 1);

    assert(solar_os_stream_unregister(foreign.info.id) == ESP_OK);
    assert(solar_os_stream_register(&foreign) == ESP_OK);
    assert(!camera->provider_close(provider) && !provider_leases && !native_allocations);
    assert(solar_os_stream_get_info(foreign.info.id, &info) == ESP_OK && info.type == SOLAR_OS_STREAM_TYPE_SCALAR);
    assert(solar_os_stream_unregister(foreign.info.id) == ESP_OK);
}

int main(void)
{
    static const solar_os_camera_backend_ops_t operations = {
        .start = fake_start,
        .set_orientation = fake_orientation,
        .stop = fake_stop,
        .capture = fake_capture,
        .release = fake_release,
        .get_resolutions = fake_resolutions,
        .control = fake_control,
        .invalidate = fake_invalidate,
    };
    fake_camera_t fake = {0};
    const solar_os_camera_backend_t backend = {
        .driver = "fake-camera",
        .ops = &operations,
        .ctx = &fake,
    };

    assert(solar_os_camera_set_orientation(SOLAR_OS_CAMERA_ORIENTATION_180) == ESP_ERR_NOT_SUPPORTED);
    solar_os_camera_backend_t bad_processor = backend;
    bad_processor.processor = "../bad";
    assert(solar_os_camera_register_backend(&bad_processor) == ESP_ERR_INVALID_ARG);
    bad_processor.processor = "processor-name-too-long";
    assert(solar_os_camera_register_backend(&bad_processor) == ESP_ERR_INVALID_ARG);
    bad_processor.processor = "test-ai";
    assert(solar_os_camera_register_backend(&bad_processor) == ESP_ERR_INVALID_ARG);
    bad_processor.processor_source = "bad/source";
    assert(solar_os_camera_register_backend(&bad_processor) == ESP_ERR_INVALID_ARG);
    assert(solar_os_camera_register_backend(&backend) == ESP_OK);
    assert(solar_os_camera_register_backend(&backend) == ESP_ERR_INVALID_STATE);
    assert(solar_os_camera_set_orientation((solar_os_camera_orientation_t)4) == ESP_ERR_INVALID_ARG);
    assert(solar_os_camera_set_orientation(SOLAR_OS_CAMERA_ORIENTATION_180) == ESP_OK);
    assert(fake.starts == 0U);

    solar_os_camera_status_t status;
    assert(solar_os_camera_get_status(&status) == ESP_OK);
    assert(status.backend_registered);
    assert(!status.initialized);
    assert(strcmp(status.driver, "fake-camera") == 0);
    assert(!*status.processor);

    solar_os_camera_config_t config = solar_os_camera_default_config();
    assert(config.width == 320U && config.height == 240U);
    assert(config.jpeg_quality == 12U);
    uint16_t width = 9, height = 9;
    assert(solar_os_camera_parse_resolution("416x416", &width, &height) && width == 416 && height == 416);
    assert(solar_os_camera_parse_resolution("qvga", &width, &height) && width == 320 && height == 240);
    assert(solar_os_camera_parse_resolution("vga", &width, &height) && width == 640 && height == 480);
    const char *invalid_sizes[] = {"", "x480", "0x240", "320x0", "65536x1", "1x99999999999", "320x240junk", "-1x2", "1x2x3"};
    for (size_t i = 0; i < sizeof(invalid_sizes) / sizeof(invalid_sizes[0]); i++)
        assert(!solar_os_camera_parse_resolution(invalid_sizes[i], &width, &height));
    solar_os_camera_resolution_t modes[1];
    size_t mode_count = 0;
    assert(solar_os_camera_get_resolutions(NULL, 0, &mode_count) == ESP_OK && mode_count == 3);
    assert(solar_os_camera_get_resolutions(modes, 1, &mode_count) == ESP_OK && mode_count == 3);
    assert(modes[0].width == 320 && modes[0].height == 240);
    assert(solar_os_camera_get_resolutions(NULL, 1, &mode_count) == ESP_ERR_INVALID_ARG);
    solar_os_camera_owner_t owner = {0};
    solar_os_camera_owner_t other = {0};
    assert(solar_os_camera_acquire("test", &owner) == ESP_OK);
    assert(owner.generation != 0U);
    assert(solar_os_camera_acquire("other", &other) == ESP_ERR_INVALID_STATE);
    assert(other.generation == 0U);
    assert(solar_os_camera_get_status(&status) == ESP_OK);
    assert(status.owner_leased);
    assert(strcmp(status.owner, "test") == 0);

    solar_os_camera_owner_t invalid = {.generation = owner.generation + 1U};
    const esp_err_t control_result = ESP_ERR_NOT_SUPPORTED;
    assert(solar_os_camera_control(&invalid, "test", &control_result) == ESP_ERR_INVALID_STATE);
    assert(solar_os_camera_control(&owner, "test", &control_result) == control_result);
    assert(solar_os_camera_control(&owner, NULL, &control_result) == ESP_ERR_INVALID_ARG);
    assert(solar_os_camera_invalidate(&invalid) == ESP_ERR_INVALID_STATE);
    assert(solar_os_camera_start(&invalid, &config) == ESP_ERR_INVALID_STATE);
    assert(solar_os_camera_start(&owner, &config) == ESP_OK);
    assert(fake.starts == 1U);
    assert(fake.orientation == SOLAR_OS_CAMERA_ORIENTATION_180);
    assert(solar_os_camera_start(&owner, &config) == ESP_OK);
    assert(fake.starts == 1U);

    const solar_os_camera_frame_t *frame = NULL;
    assert(solar_os_camera_capture(&owner, &frame) == ESP_OK);
    assert(frame != NULL);
    assert(frame->data == fake_jpeg);
    assert(frame->width == 320U && frame->height == 240U);
    const solar_os_camera_frame_t *leased_frame = frame;
    assert(solar_os_camera_control(&owner, "test", &control_result) == ESP_ERR_INVALID_STATE);
    assert(solar_os_camera_invalidate(&owner) == ESP_ERR_INVALID_STATE && !invalidations);
    /* Live controls leave the capture owner and its existing JPEG valid. */
    assert(solar_os_camera_set_orientation(SOLAR_OS_CAMERA_ORIENTATION_MIRROR) == ESP_OK);
    assert(solar_os_camera_get_status(&status) == ESP_OK);
    assert(status.owner_leased && status.frame_leased);
    assert(status.orientation == SOLAR_OS_CAMERA_ORIENTATION_MIRROR);
    assert(strcmp(status.owner, "test") == 0);
    const solar_os_camera_frame_t *second_frame = NULL;
    assert(solar_os_camera_capture(&owner, &second_frame) == ESP_ERR_INVALID_STATE);
    assert(second_frame == NULL);
    assert(solar_os_camera_start(&owner, &config) == ESP_ERR_INVALID_STATE);
    solar_os_camera_frame_t unrelated = {0};
    assert(solar_os_camera_release_frame(&owner, &unrelated) == ESP_ERR_INVALID_ARG);
    assert(solar_os_camera_release_frame(&invalid, leased_frame) == ESP_ERR_INVALID_STATE);
    assert(solar_os_camera_release_owner(&owner) == ESP_ERR_INVALID_STATE);
    assert(solar_os_camera_release_frame(&owner, leased_frame) == ESP_OK);
    assert(fake.releases == 1U);

    config.width = 640U; config.height = 480U;
    assert(solar_os_camera_start(&owner, &config) == ESP_OK);
    assert(fake.stops == 1U);
    assert(fake.starts == 2U);
    assert(fake.orientation == SOLAR_OS_CAMERA_ORIENTATION_MIRROR);
    assert(solar_os_camera_capture(&owner, &frame) == ESP_OK);
    assert(frame->width == 640U && frame->height == 480U);
    assert(solar_os_camera_release_frame(&owner, frame) == ESP_OK);

    fake.fail_capture = true;
    assert(solar_os_camera_capture(&owner, &frame) == ESP_ERR_TIMEOUT);
    assert(solar_os_camera_get_status(&status) == ESP_OK);
    assert(status.capture_count == 2U);
    assert(status.last_error == ESP_ERR_TIMEOUT);
    assert(strcmp(status.sensor.name, "OV2640") == 0);

    assert(solar_os_camera_stop(&owner) == ESP_OK);
    assert(fake.stops == 2U);
    assert(solar_os_camera_get_status(&status) == ESP_OK);
    assert(!status.initialized && status.orientation == SOLAR_OS_CAMERA_ORIENTATION_MIRROR);
    const uint32_t first_generation = owner.generation;
    assert(solar_os_camera_release_owner(&owner) == ESP_OK);
    assert(owner.generation == 0U);
    assert(solar_os_camera_start(&owner, &config) == ESP_ERR_INVALID_STATE);
    assert(solar_os_camera_acquire("other", &other) == ESP_OK);
    assert(other.generation != first_generation);
    assert(solar_os_camera_release_owner(&other) == ESP_OK);
    assert(solar_os_camera_unregister_backend("fake-camera") == ESP_OK);
    /* Defaults are copied, and arbitrary dimensions survive start/capture. */
    solar_os_camera_config_t square = {.width = 416, .height = 416, .jpeg_quality = 0};
    solar_os_camera_backend_t square_backend = backend;
    char processor[] = "test-ai";
    char processor_source[] = "camera-test";
    square_backend.processor = processor;
    square_backend.processor_source = processor_source;
    square_backend.default_config = &square;
    assert(solar_os_camera_register_backend(&square_backend) == ESP_OK);
    processor[0] = 'x';
    processor_source[0] = 'x';
    assert(solar_os_camera_get_status(&status) == ESP_OK);
    assert(!strcmp(status.processor, "test-ai"));
    assert(!strcmp(status.processor_source, "camera-test"));
    square.width = 999;
    config = solar_os_camera_default_config();
    assert(config.width == 416 && config.height == 416 && config.jpeg_quality == 0);
    fake.fail_capture = false;
    assert(solar_os_camera_acquire("square", &owner) == ESP_OK);
    assert(solar_os_camera_start(&owner, &config) == ESP_OK);
    assert(solar_os_camera_capture(&owner, &frame) == ESP_OK && frame->width == 416 && frame->height == 416);
    assert(solar_os_camera_release_frame(&owner, frame) == ESP_OK);
    unsigned starts_before = fake.starts;
    assert(solar_os_camera_invalidate(&owner) == ESP_OK && invalidations == 1);
    assert(solar_os_camera_get_status(&status) == ESP_OK && !status.initialized);
    assert(solar_os_camera_start(&owner, &config) == ESP_OK && fake.starts == starts_before + 1);
    assert(solar_os_camera_stop(&owner) == ESP_OK);
    assert(solar_os_camera_release_owner(&owner) == ESP_OK);
    assert(solar_os_camera_unregister_backend("fake-camera") == ESP_OK);
    config = solar_os_camera_default_config();
    assert(config.width == 320 && config.height == 240 && config.jpeg_quality == 12);
    assert(solar_os_camera_get_status(&status) == ESP_OK);
    assert(!status.backend_registered);
    assert(!*status.processor);
    assert(!*status.processor_source);
    assert(status.orientation == SOLAR_OS_CAMERA_ORIENTATION_NORMAL);
    solar_os_camera_backend_ops_t unsupported_ops = operations;
    unsupported_ops.set_orientation = NULL;
    solar_os_camera_backend_t unsupported = backend;
    unsupported.ops = &unsupported_ops;
    assert(solar_os_camera_register_backend(&unsupported) == ESP_OK);
    assert(solar_os_camera_set_orientation(SOLAR_OS_CAMERA_ORIENTATION_MIRROR) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_camera_unregister_backend("fake-camera") == ESP_OK);
    test_native_camera(&operations);
    test_native_camera_provider();
    puts("camera service tests: ok");
    return 0;
}
