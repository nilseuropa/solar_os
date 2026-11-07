#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_camera.h"

typedef struct {
    bool started;
    bool fail_capture;
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
        .width = fake->config.frame_size == SOLAR_OS_CAMERA_FRAME_SIZE_VGA ?
            640U : 320U,
        .height = fake->config.frame_size == SOLAR_OS_CAMERA_FRAME_SIZE_VGA ?
            480U : 240U,
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

int main(void)
{
    static const solar_os_camera_backend_ops_t operations = {
        .start = fake_start,
        .set_orientation = fake_orientation,
        .stop = fake_stop,
        .capture = fake_capture,
        .release = fake_release,
    };
    fake_camera_t fake = {0};
    const solar_os_camera_backend_t backend = {
        .driver = "fake-camera",
        .ops = &operations,
        .ctx = &fake,
    };

    assert(solar_os_camera_set_orientation(SOLAR_OS_CAMERA_ORIENTATION_180) == ESP_ERR_NOT_SUPPORTED);
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

    solar_os_camera_config_t config = solar_os_camera_default_config();
    assert(config.frame_size == SOLAR_OS_CAMERA_FRAME_SIZE_QVGA);
    assert(config.jpeg_quality == 12U);
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

    config.frame_size = SOLAR_OS_CAMERA_FRAME_SIZE_VGA;
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
    assert(solar_os_camera_get_status(&status) == ESP_OK);
    assert(!status.backend_registered);
    assert(status.orientation == SOLAR_OS_CAMERA_ORIENTATION_NORMAL);
    solar_os_camera_backend_ops_t unsupported_ops = operations;
    unsupported_ops.set_orientation = NULL;
    solar_os_camera_backend_t unsupported = backend;
    unsupported.ops = &unsupported_ops;
    assert(solar_os_camera_register_backend(&unsupported) == ESP_OK);
    assert(solar_os_camera_set_orientation(SOLAR_OS_CAMERA_ORIENTATION_MIRROR) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_camera_unregister_backend("fake-camera") == ESP_OK);
    puts("camera service tests: ok");
    return 0;
}
