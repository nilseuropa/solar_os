#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_camera.h"
#include "i2c_bus.h"
#include "solar_os_buses.h"
#include "solar_os_camera_esp32.h"
#include "solar_os_camera.h"
#include "solar_os_camera_stream.h"
#include "solar_os_memory.h"
#include "solar_os_resources.h"
#include "solar_os_stream.h"

const camera_sensor_info_t camera_sensor[] = {{0x26, "OV2640"}};
static sensor_t sensor = {.id.PID = 0x26};
static camera_config_t native;
static uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
static camera_fb_t fb = {.buf = jpeg, .len = sizeof(jpeg), .format = PIXFORMAT_JPEG,
                        .timestamp = {.tv_sec = 1, .tv_usec = 1234}};
static unsigned starts, stops, releases, allocations;
static bool fail_start, fail_stop, fail_capture;
static unsigned bus_lookups, bus_locks;
static bool bus_locked;
static esp_err_t bus_result = ESP_OK;

esp_err_t solar_os_bus_i2c_get_handle(const char *name,
                                      i2c_master_bus_handle_t *handle, int *port)
{
    assert(strcmp(name, "i2c0") == 0);
    bus_lookups++;
    if (bus_result != ESP_OK) return bus_result;
    *handle = (void *)1;
    *port = 0;
    return ESP_OK;
}
void i2c_bus_lock(void) { assert(!bus_locked); bus_locked = true; bus_locks++; }
void i2c_bus_unlock(void) { assert(bus_locked); bus_locked = false; }

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size > 0U) {
        const size_t n = len < size - 1U ? len : size - 1U;
        memcpy(dst, src, n); dst[n] = '\0';
    }
    return len;
}
uint64_t solar_os_time_uptime_ms(void) { return 1234U; }
esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *value)
{ *value = 0U; return ESP_OK; }
void *solar_os_memory_calloc(size_t count, size_t size,
                             solar_os_memory_class_t cls, const char *tag)
{
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED);
    assert(strcmp(tag, "camera.device") == 0);
    allocations++;
    return calloc(count, size);
}
void solar_os_memory_free(void *ptr) { assert(allocations > 0U); allocations--; free(ptr); }
esp_err_t esp_camera_init(const camera_config_t *config)
{
    assert(bus_locked == (config->pin_sccb_sda == -1));
    if (fail_start) return ESP_ERR_NO_MEM;
    native = *config; starts++;
    fb.width = native.frame_size == FRAMESIZE_VGA ? 640 : 320;
    fb.height = native.frame_size == FRAMESIZE_VGA ? 480 : 240;
    return ESP_OK;
}
esp_err_t esp_camera_deinit(void)
{ assert(bus_locked == (native.pin_sccb_sda == -1)); if (fail_stop) return ESP_FAIL; stops++; return ESP_OK; }
sensor_t *esp_camera_sensor_get(void) { return &sensor; }
camera_fb_t *esp_camera_fb_get(void) { return fail_capture ? NULL : &fb; }
void esp_camera_fb_return(camera_fb_t *frame) { assert(frame == &fb); releases++; }

static esp_err_t pending_open(void *user, const char *owner,
                               const solar_os_stream_open_options_t *options,
                               solar_os_stream_handle_t *handle)
{
    (void)user; (void)owner; (void)options; (void)handle;
    /* Simulate the window after registry reservation but before camera lease. */
    assert(solar_os_camera_esp32_expansion_driver.detach("camera0") == ESP_ERR_INVALID_STATE);
    return ESP_FAIL;
}
static esp_err_t unused_frame(void *user, solar_os_stream_handle_t *handle,
                               solar_os_stream_video_frame_t *frame)
{ (void)user; (void)handle; (void)frame; return ESP_ERR_NOT_SUPPORTED; }

int main(void)
{
    const solar_os_expansion_driver_t *driver = &solar_os_camera_esp32_expansion_driver;
    assert(driver->binding_spec_count == 17U && driver->early);
    solar_os_expansion_binding_t pins[16] = {0};
    const int wiring[] = {11, 9, 8, 10, 12, 18, 17, 16, 4, 5, 6, 7, 13, 15};
    for (size_t i = 0U; i < 14U; i++) {
        pins[i].kind = SOLAR_OS_EXPANSION_BINDING_GPIO;
        strcpy(pins[i].role, driver->binding_specs[i].role);
        pins[i].value = wiring[i];
    }
    /* Missing, incomplete and mixed SCCB modes fail before reserving hardware. */
    solar_os_expansion_binding_t shared[16] = {0};
    memcpy(shared, pins, 8U * sizeof(*shared));
    memcpy(shared + 8U, pins + 10U, 4U * sizeof(*shared));
    shared[12].kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS;
    strcpy(shared[12].target, "i2c0");
    solar_os_expansion_binding_validation_t validation;
    assert(driver->validate_bindings(shared, 12U, &validation) == ESP_ERR_INVALID_ARG);
    assert(validation.reason == SOLAR_OS_EXPANSION_BINDINGS_INVALID_VALUE);
    assert(driver->attach("camera0", shared, 12U) == ESP_ERR_INVALID_ARG);
    shared[13] = pins[8];
    assert(driver->attach("camera0", shared, 14U) == ESP_ERR_INVALID_ARG);
    shared[12] = pins[8];
    assert(driver->attach("camera0", shared, 13U) == ESP_ERR_INVALID_ARG);
    shared[12] = pins[9];
    assert(driver->attach("camera0", shared, 13U) == ESP_ERR_INVALID_ARG);
    assert(allocations == 0U && solar_os_resource_claim_count() == 0U);

    /* Direct pins must not steal a controller already owned by a named bus. */
    assert(solar_os_resource_claim(SOLAR_OS_RESOURCE_I2C_PORT, 1, -1,
                                   "i2c1", "shared bus") == ESP_OK);
    assert(driver->attach("camera0", pins, 14U) == ESP_ERR_INVALID_STATE);
    assert(allocations == 0U && solar_os_resource_claim_count() == 1U);
    solar_os_resource_release_owner("i2c1");

    /* A conflicting source rolls back backend/config/peripheral registration. */
    assert(solar_os_camera_stream_register("camera0") == ESP_OK);
    assert(driver->attach("camera0", pins, 14U) == ESP_ERR_INVALID_STATE);
    assert(allocations == 0U && solar_os_resource_claim_count() == 0U);
    solar_os_camera_status_t status;
    assert(solar_os_camera_get_status(&status) == ESP_OK && !status.backend_registered);
    assert(solar_os_camera_stream_unregister("camera0") == ESP_OK);

    assert(driver->attach("camera0", pins, 14U) == ESP_OK);
    assert(allocations == 1U && starts == 0U);
    assert(driver->attach("camera1", pins, 14U) == ESP_ERR_INVALID_STATE);
    solar_os_stream_info_t info;
    assert(solar_os_stream_get_info("camera0", &info) == ESP_OK);
    assert(info.type == SOLAR_OS_STREAM_TYPE_VIDEO && info.active_handles == 0U);
    assert(strcmp(info.format, "jpeg") == 0 && starts == 0U);
    char csv_header[96];
    assert(solar_os_stream_csv_header(&info, csv_header, sizeof(csv_header)) == ESP_ERR_NOT_SUPPORTED);

    solar_os_stream_handle_t handle = SOLAR_OS_STREAM_HANDLE_INIT;
    solar_os_stream_handle_t second = SOLAR_OS_STREAM_HANDLE_INIT;
    assert(solar_os_camera_stream_unregister("camera0") == ESP_OK);
    const solar_os_stream_driver_t pending = {.info = info, .open = pending_open,
        .acquire_frame = unused_frame, .release_frame = unused_frame};
    assert(solar_os_stream_register(&pending) == ESP_OK);
    assert(solar_os_stream_open("camera0", "pending", &handle) == ESP_FAIL);
    assert(solar_os_camera_get_status(&status) == ESP_OK && status.backend_registered);
    assert(solar_os_stream_unregister("camera0") == ESP_OK);
    assert(solar_os_camera_stream_register("camera0") == ESP_OK);
    solar_os_stream_open_options_t options = {.direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
        .requested_video = {.width = 640, .height = 480, .jpeg_quality = 15}};
    options.requested_video.width = 800;
    assert(solar_os_stream_open_ex("camera0", "viewer", &options, &handle) == ESP_ERR_NOT_SUPPORTED);
    options.requested_video.width = 640;
    fail_start = true;
    assert(solar_os_stream_open_ex("camera0", "viewer", &options, &handle) == ESP_ERR_NO_MEM);
    assert(solar_os_camera_get_status(&status) == ESP_OK && !status.owner_leased);
    fail_start = false;
    assert(solar_os_stream_open_ex("camera0", "viewer", &options, &handle) == ESP_OK);
    assert(native.pin_d0 == 11 && native.pin_d1 == 9 && native.pin_d2 == 8);
    assert(native.pin_d3 == 10 && native.pin_d4 == 12 && native.pin_d5 == 18);
    assert(native.pin_d6 == 17 && native.pin_d7 == 16);
    assert(native.pin_sccb_sda == 4 && native.pin_sccb_scl == 5);
    assert(native.sccb_i2c_port == -1 && bus_lookups == 0U);
    assert(native.pin_vsync == 6 && native.pin_href == 7);
    assert(native.pin_pclk == 13 && native.pin_xclk == 15);
    assert(native.pin_pwdn == -1 && native.pin_reset == -1);
    assert(native.fb_count == 1 && native.fb_location == CAMERA_FB_IN_PSRAM);
    assert(handle.video.width == 640 && handle.video.height == 480);
    assert(solar_os_stream_open("camera0", "other", &second) == ESP_ERR_INVALID_STATE);
    solar_os_camera_owner_t owner = {0};
    assert(solar_os_camera_acquire("cam-webd", &owner) == ESP_ERR_INVALID_STATE);
    assert(driver->detach("camera0") == ESP_ERR_INVALID_STATE);
    assert(solar_os_stream_get_info("camera0", &info) == ESP_OK);
    assert(info.active_handles == 1U && strcmp(info.owner, "viewer") == 0);
    solar_os_stream_video_frame_t frame = {0}, other = {0};
    fail_capture = true;
    assert(solar_os_stream_acquire_frame(&handle, &frame) == ESP_ERR_TIMEOUT);
    fail_capture = false;
    assert(solar_os_stream_acquire_frame(&handle, &frame) == ESP_OK);
    assert(frame.data == jpeg && frame.timestamp_us == 1001234U);
    assert(solar_os_stream_acquire_frame(&handle, &other) == ESP_ERR_INVALID_STATE);
    assert(solar_os_stream_close_ex(&handle) == ESP_ERR_INVALID_STATE);
    assert(solar_os_stream_open("camera0", "replacement", &handle) == ESP_ERR_INVALID_STATE);
    solar_os_stream_close(&handle);
    assert(solar_os_stream_handle_valid(&handle));
    assert(solar_os_stream_release_frame(&handle, &other) == ESP_ERR_INVALID_STATE);
    assert(solar_os_stream_release_frame(&handle, &frame) == ESP_OK && releases == 1U);
    assert(frame.data == NULL && solar_os_stream_release_frame(&handle, &frame) == ESP_ERR_INVALID_STATE);
    fail_stop = true;
    assert(solar_os_stream_close_ex(&handle) == ESP_FAIL);
    assert(solar_os_stream_handle_valid(&handle));
    fail_stop = false;
    assert(solar_os_stream_close_ex(&handle) == ESP_OK && stops == 1U);

    /* Direct service users still hold the same exclusive owner token. */
    assert(solar_os_camera_acquire("cam-webd", &owner) == ESP_OK);
    assert(solar_os_stream_open("camera0", "viewer", &handle) == ESP_ERR_INVALID_STATE);
    assert(driver->detach("camera0") == ESP_ERR_INVALID_STATE);
    assert(solar_os_camera_release_owner(&owner) == ESP_OK);
    assert(driver->detach("camera0") == ESP_OK && allocations == 0U);
    solar_os_resource_release_owner("camera0"); /* Registry's detach cleanup. */
    assert(solar_os_stream_get_info("camera0", &info) == ESP_ERR_NOT_FOUND);
    assert(solar_os_resource_claim_count() == 0U);
    for (size_t i = 14U; i < 16U; i++) {
        pins[i].kind = SOLAR_OS_EXPANSION_BINDING_GPIO;
        strcpy(pins[i].role, driver->binding_specs[i].role);
        pins[i].value = i == 14U ? 21 : 47;
    }
    assert(driver->attach("camera0", pins, 16U) == ESP_OK);
    assert(solar_os_stream_open("camera0", "viewer", &handle) == ESP_OK);
    assert(native.pin_pwdn == 21 && native.pin_reset == 47);
    assert(handle.video.width == 320U && handle.video.height == 240U);
    assert(solar_os_stream_close_ex(&handle) == ESP_OK);
    assert(driver->detach("camera0") == ESP_OK);
    solar_os_resource_release_owner("camera0");
    assert(allocations == 0U);
    /* Shared mode uses port 0, leaves SCCB pins unset and never claims its port. */
    shared[12].kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS;
    shared[12].role[0] = '\0';
    strcpy(shared[12].target, "i2c0");
    assert(solar_os_resource_claim(SOLAR_OS_RESOURCE_I2C_PORT, 0, -1,
                                   "i2c0", "board bus") == ESP_OK);
    assert(driver->validate_bindings(shared, 13U, &validation) == ESP_OK);
    assert(driver->attach("camera0", shared, 13U) == ESP_OK);
    assert(solar_os_resource_claim_count() == 2U); /* board bus plus DVP */
    bus_result = ESP_ERR_INVALID_STATE;
    assert(solar_os_stream_open("camera0", "viewer", &handle) == ESP_ERR_INVALID_STATE);
    assert(!bus_locked);
    bus_result = ESP_OK;
    fail_start = true;
    assert(solar_os_stream_open("camera0", "viewer", &handle) == ESP_ERR_NO_MEM);
    assert(!bus_locked);
    fail_start = false;
    assert(solar_os_stream_open("camera0", "viewer", &handle) == ESP_OK);
    assert(native.pin_sccb_sda == -1 && native.pin_sccb_scl == -1);
    assert(native.sccb_i2c_port == 0);
    assert(solar_os_stream_acquire_frame(&handle, &frame) == ESP_OK);
    assert(solar_os_stream_release_frame(&handle, &frame) == ESP_OK);
    fail_stop = true;
    assert(solar_os_stream_close_ex(&handle) == ESP_FAIL);
    assert(!bus_locked && solar_os_stream_handle_valid(&handle));
    fail_stop = false;
    assert(solar_os_stream_close_ex(&handle) == ESP_OK);
    assert(solar_os_stream_open("camera0", "viewer", &handle) == ESP_OK);
    assert(solar_os_stream_close_ex(&handle) == ESP_OK);
    assert(driver->detach("camera0") == ESP_OK);
    solar_os_resource_release_owner("camera0");
    assert(solar_os_resource_claim_count() == 1U && allocations == 0U);
    assert(bus_lookups == 4U && bus_locks > 0U && !bus_locked);
    solar_os_resource_release_owner("i2c0");
    puts("camera expansion and typed-frame stream tests: ok");
    return 0;
}
