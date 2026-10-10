#include "solar_os_camera_esp32.h"

#include <string.h>

#include "driver/ledc.h"
#include "esp_camera.h"
#include "sdkconfig.h"
#include "solar_os_board.h"
#if SOLAR_OS_BOARD_HAS_I2C
#include "i2c_bus.h"
#endif
#include "solar_os_buses.h"
#include "solar_os_camera.h"
#include "solar_os_camera_stream.h"
#include "solar_os_memory.h"
#include "solar_os_resources.h"

#define CAMERA_XCLK_HZ 20000000
#define CAMERA_LEDC_TIMER LEDC_TIMER_1
#define CAMERA_LEDC_CHANNEL LEDC_CHANNEL_4
#if CONFIG_SCCB_HARDWARE_I2C_PORT1
#define CAMERA_SCCB_PORT 1
#else
#define CAMERA_SCCB_PORT 0
#endif

typedef struct {
    bool started;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int pins[16];
} camera_esp32_state_t;

/* esp32-camera has one hardware instance; config lives in PSRAM only while
 * attached. Timer 1/channel 4 are separate from PWM (0/0..3) and audio (3/7). */
static camera_esp32_state_t *camera_esp32;

static void camera_sccb_lock(void)
{
#if SOLAR_OS_BOARD_HAS_I2C
    i2c_bus_lock();
#endif
}

static void camera_sccb_unlock(void)
{
#if SOLAR_OS_BOARD_HAS_I2C
    i2c_bus_unlock();
#endif
}

enum { PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5, PIN_D6, PIN_D7,
       PIN_SIOD, PIN_SIOC, PIN_VSYNC, PIN_HREF, PIN_PCLK, PIN_XCLK,
       PIN_PWDN, PIN_RESET };

#define CAMERA_PIN(key_, required_) \
    {.key = key_, .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, \
     .role = key_, .required = required_}
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    CAMERA_PIN("d0", true), CAMERA_PIN("d1", true), CAMERA_PIN("d2", true),
    CAMERA_PIN("d3", true), CAMERA_PIN("d4", true), CAMERA_PIN("d5", true),
    CAMERA_PIN("d6", true), CAMERA_PIN("d7", true), CAMERA_PIN("siod", false),
    CAMERA_PIN("sioc", false), CAMERA_PIN("vsync", true), CAMERA_PIN("href", true),
    CAMERA_PIN("pclk", true), CAMERA_PIN("xclk", true), CAMERA_PIN("pwdn", false),
    CAMERA_PIN("reset", false),
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS},
};
_Static_assert(sizeof(binding_specs) / sizeof(binding_specs[0]) == 17U,
               "camera pin order must match binding specs");

static esp_err_t camera_validate_bindings(
    const solar_os_expansion_binding_t *bindings, size_t count,
    solar_os_expansion_binding_validation_t *validation)
{
    bool bus = false, siod = false, sioc = false;
    for (size_t i = 0U; i < count; i++) {
        if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS) bus = true;
        if (bindings[i].kind != SOLAR_OS_EXPANSION_BINDING_GPIO) continue;
        if (strcmp(bindings[i].role, "siod") == 0) siod = true;
        if (strcmp(bindings[i].role, "sioc") == 0) sioc = true;
    }
    if ((bus && !siod && !sioc) || (!bus && siod && sioc)) return ESP_OK;
    if (validation != NULL) {
        validation->reason = SOLAR_OS_EXPANSION_BINDINGS_INVALID_VALUE;
        strlcpy(validation->key, "i2c/siod/sioc", sizeof(validation->key));
    }
    return ESP_ERR_INVALID_ARG;
}

static framesize_t native_frame_size(solar_os_camera_frame_size_t frame_size)
{
    return frame_size == SOLAR_OS_CAMERA_FRAME_SIZE_VGA ?
        FRAMESIZE_VGA : FRAMESIZE_QVGA;
}

static const char *sensor_name(uint16_t product_id)
{
    for (size_t index = 0U; index < CAMERA_MODEL_MAX; index++) {
        if ((uint16_t)camera_sensor[index].pid == product_id) {
            return camera_sensor[index].name;
        }
    }
    return "unknown";
}

static esp_err_t camera_start(void *ctx,
                              const solar_os_camera_config_t *config,
                              solar_os_camera_sensor_info_t *sensor_info)
{
    camera_esp32_state_t *state = ctx;
    if (state->started) {
        return ESP_ERR_INVALID_STATE;
    }

    int sccb_port = -1;
    if (state->i2c_bus[0] != '\0') {
        i2c_master_bus_handle_t bus_handle;
        /* The expansion registry holds this lease until camera detach. */
        const esp_err_t error = solar_os_bus_i2c_get_handle(state->i2c_bus,
                                                           &bus_handle,
                                                           &sccb_port);
        if (error != ESP_OK) return error;
    }
    const camera_config_t native_config = {
        .pin_pwdn = state->pins[PIN_PWDN],
        .pin_reset = state->pins[PIN_RESET],
        .pin_xclk = state->pins[PIN_XCLK],
        .pin_sccb_sda = state->pins[PIN_SIOD],
        .pin_sccb_scl = state->pins[PIN_SIOC],
        .pin_d7 = state->pins[PIN_D7], .pin_d6 = state->pins[PIN_D6],
        .pin_d5 = state->pins[PIN_D5], .pin_d4 = state->pins[PIN_D4],
        .pin_d3 = state->pins[PIN_D3], .pin_d2 = state->pins[PIN_D2],
        .pin_d1 = state->pins[PIN_D1], .pin_d0 = state->pins[PIN_D0],
        .pin_vsync = state->pins[PIN_VSYNC],
        .pin_href = state->pins[PIN_HREF],
        .pin_pclk = state->pins[PIN_PCLK],
        .xclk_freq_hz = CAMERA_XCLK_HZ,
        .ledc_timer = CAMERA_LEDC_TIMER,
        .ledc_channel = CAMERA_LEDC_CHANNEL,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = native_frame_size(config->frame_size),
        .jpeg_quality = config->jpeg_quality,
        .fb_count = 1U,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        .sccb_i2c_port = sccb_port,
    };

    /* Shared SCCB adds/removes IDF devices outside SolarOS transfer helpers.
     * Its bus lease guarantees that the I2C mutex has been initialized. */
    const bool shared_sccb = state->i2c_bus[0] != '\0';
    if (shared_sccb) camera_sccb_lock();
    const esp_err_t error = esp_camera_init(&native_config);
    if (error != ESP_OK) {
        if (shared_sccb) camera_sccb_unlock();
        return error;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        (void)esp_camera_deinit();
        if (shared_sccb) camera_sccb_unlock();
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (shared_sccb) camera_sccb_unlock();

    state->started = true;
    sensor_info->product_id = sensor->id.PID;
    strlcpy(sensor_info->name, sensor_name(sensor->id.PID),
            sizeof(sensor_info->name));
    return ESP_OK;
}

static esp_err_t camera_stop(void *ctx)
{
    camera_esp32_state_t *state = ctx;
    if (!state->started) {
        return ESP_OK;
    }
    const bool shared_sccb = state->i2c_bus[0] != '\0';
    if (shared_sccb) camera_sccb_lock();
    const esp_err_t error = esp_camera_deinit();
    if (shared_sccb) camera_sccb_unlock();
    if (error == ESP_OK) {
        state->started = false;
    }
    return error;
}

static esp_err_t camera_capture(void *ctx,
                                solar_os_camera_backend_frame_t *frame)
{
    camera_esp32_state_t *state = ctx;
    if (!state->started) {
        return ESP_ERR_INVALID_STATE;
    }
    camera_fb_t *native_frame = esp_camera_fb_get();
    if (native_frame == NULL) {
        return ESP_ERR_TIMEOUT;
    }
    if (native_frame->format != PIXFORMAT_JPEG || native_frame->buf == NULL ||
        native_frame->len < 4U || native_frame->buf[0] != 0xffU ||
        native_frame->buf[1] != 0xd8U ||
        native_frame->buf[native_frame->len - 2U] != 0xffU ||
        native_frame->buf[native_frame->len - 1U] != 0xd9U) {
        esp_camera_fb_return(native_frame);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *frame = (solar_os_camera_backend_frame_t) {
        .data = native_frame->buf,
        .length = native_frame->len,
        .width = native_frame->width,
        .height = native_frame->height,
        .timestamp_us = (uint64_t)native_frame->timestamp.tv_sec * 1000000ULL +
            (uint64_t)native_frame->timestamp.tv_usec,
        .release_token = native_frame,
    };
    return ESP_OK;
}

static void camera_release(void *ctx, void *release_token)
{
    (void)ctx;
    esp_camera_fb_return(release_token);
}

static esp_err_t camera_publish(void *ctx)
{
    return solar_os_camera_stream_register(((camera_esp32_state_t *)ctx)->name);
}

static esp_err_t camera_unpublish(void *ctx)
{
    return solar_os_camera_stream_unregister(((camera_esp32_state_t *)ctx)->name);
}

static esp_err_t camera_attach(const char *name,
                               const solar_os_expansion_binding_t *bindings,
                               size_t count)
{
    if (camera_esp32 != NULL) return ESP_ERR_INVALID_STATE;
    if (camera_validate_bindings(bindings, count, NULL) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    camera_esp32_state_t *state = solar_os_memory_calloc(
        1U, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "camera.device");
    if (state == NULL) return ESP_ERR_NO_MEM;
    strlcpy(state->name, name, sizeof(state->name));
    for (size_t i = 0U; i < 16U; i++) state->pins[i] = -1;
    for (size_t i = 0U; i < count; i++) {
        if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS) {
            strlcpy(state->i2c_bus, bindings[i].target, sizeof(state->i2c_bus));
            continue;
        }
        for (size_t j = 0U; j < 16U; j++) {
            if (strcmp(bindings[i].role, binding_specs[j].role) == 0) {
                state->pins[j] = bindings[i].value;
            }
        }
    }
    /* Direct-pin SCCB uses Espressif's Kconfig-selected controller. */
    const solar_os_resource_request_t requests[] = {
        {SOLAR_OS_RESOURCE_CAMERA_PORT, 0, -1, "DVP capture"},
        {SOLAR_OS_RESOURCE_I2C_PORT, CAMERA_SCCB_PORT, -1, "camera SCCB"},
    };
    esp_err_t error = solar_os_resource_claim_bundle(requests,
        state->i2c_bus[0] == '\0' ? 2U : 1U, name, NULL);
    if (error != ESP_OK) {
        solar_os_memory_free(state);
        return error;
    }
    static const solar_os_camera_backend_ops_t operations = {
        .start = camera_start,
        .stop = camera_stop,
        .capture = camera_capture,
        .release = camera_release,
        .publish = camera_publish,
        .unpublish = camera_unpublish,
    };
    const solar_os_camera_backend_t backend = {
        .driver = "esp32-camera",
        .ops = &operations,
        .ctx = state,
    };
    error = solar_os_camera_register_backend(&backend);
    if (error != ESP_OK) {
        (void)solar_os_resource_release(SOLAR_OS_RESOURCE_CAMERA_PORT, 0, -1, name);
        if (state->i2c_bus[0] == '\0') {
            (void)solar_os_resource_release(SOLAR_OS_RESOURCE_I2C_PORT,
                                             CAMERA_SCCB_PORT, -1, name);
        }
        solar_os_memory_free(state);
        return error;
    }
    camera_esp32 = state;
    return ESP_OK;
}

static esp_err_t camera_detach(const char *name)
{
    if (camera_esp32 == NULL || strcmp(camera_esp32->name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t error = solar_os_camera_unregister_backend("esp32-camera");
    if (error != ESP_OK) return error;
    solar_os_memory_free(camera_esp32);
    camera_esp32 = NULL;
    return ESP_OK;
}

const solar_os_expansion_driver_t solar_os_camera_esp32_expansion_driver = {
    .name = "esp32-camera", .summary = "DVP JPEG camera",
    .category = SOLAR_OS_EXPANSION_CATEGORY_SENSOR,
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_GPIO | SOLAR_OS_BOARD_CAP_PSRAM,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .validate_bindings = camera_validate_bindings,
    .attach = camera_attach, .detach = camera_detach,
};
