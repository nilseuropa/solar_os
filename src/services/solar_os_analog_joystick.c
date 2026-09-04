#include "solar_os_analog_joystick.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_input.h"
#include "solar_os_stream.h"
#include "solar_os_task.h"

#define ANALOG_JOYSTICK_DEVICE_MAX 4U
#define ANALOG_JOYSTICK_POLL_MS 10U
#define ANALOG_JOYSTICK_TASK_STACK 3072U
#define ANALOG_JOYSTICK_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define ANALOG_JOYSTICK_MIN_CHANGE 256

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char x_stream[SOLAR_OS_EXPANSION_TARGET_MAX];
    char y_stream[SOLAR_OS_EXPANSION_TARGET_MAX];
    int minimum;
    int center;
    int maximum;
    int deadzone;
    int16_t last[2];
    bool last_valid[2];
    uint32_t samples;
    uint32_t dropped;
    uint32_t read_errors;
    solar_os_stream_handle_t streams[2];
    solar_os_input_source_t input_source;
    TaskHandle_t worker_task;
} solar_os_analog_joystick_device_t;

static const char *TAG = "analog-joystick";
static EXT_RAM_BSS_ATTR solar_os_analog_joystick_device_t
    devices[ANALOG_JOYSTICK_DEVICE_MAX];

static solar_os_analog_joystick_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < ANALOG_JOYSTICK_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

static solar_os_analog_joystick_device_t *alloc_device(void)
{
    for (size_t i = 0; i < ANALOG_JOYSTICK_DEVICE_MAX; i++) {
        if (!devices[i].active) {
            return &devices[i];
        }
    }
    return NULL;
}

static esp_err_t parse_bindings(
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count,
    solar_os_analog_joystick_device_t *device)
{
    bool have_x = false;
    bool have_y = false;
    bool have_minimum = false;
    bool have_center = false;
    bool have_maximum = false;

    if (bindings == NULL || device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM) {
            if (strcmp(binding->role, "x") == 0 && !have_x) {
                strlcpy(device->x_stream,
                        binding->target,
                        sizeof(device->x_stream));
                have_x = true;
            } else if (strcmp(binding->role, "y") == 0 && !have_y) {
                strlcpy(device->y_stream,
                        binding->target,
                        sizeof(device->y_stream));
                have_y = true;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER) {
            if (strcmp(binding->role, "min") == 0 && !have_minimum) {
                device->minimum = binding->value;
                have_minimum = true;
            } else if (strcmp(binding->role, "center") == 0 && !have_center) {
                device->center = binding->value;
                have_center = true;
            } else if (strcmp(binding->role, "max") == 0 && !have_maximum) {
                device->maximum = binding->value;
                have_maximum = true;
            } else if (strcmp(binding->role, "deadzone") == 0) {
                device->deadzone = binding->value;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (!have_x || !have_y || !have_minimum || !have_center || !have_maximum ||
        device->minimum >= device->center || device->center >= device->maximum ||
        device->deadzone < 0 ||
        device->center - device->deadzone <= device->minimum ||
        device->center + device->deadzone >= device->maximum) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static int16_t normalize_axis(const solar_os_analog_joystick_device_t *device,
                              float sample)
{
    const float low_edge = (float)(device->center - device->deadzone);
    const float high_edge = (float)(device->center + device->deadzone);
    if (sample <= (float)device->minimum) {
        return INT16_MIN + 1;
    }
    if (sample >= (float)device->maximum) {
        return INT16_MAX;
    }
    if (sample >= low_edge && sample <= high_edge) {
        return 0;
    }
    if (sample < low_edge) {
        const float ratio = (low_edge - sample) /
            (low_edge - (float)device->minimum);
        return (int16_t)(-32767.0f * ratio);
    }
    const float ratio = (sample - high_edge) /
        ((float)device->maximum - high_edge);
    return (int16_t)(32767.0f * ratio);
}

static void publish_axis(solar_os_analog_joystick_device_t *device,
                         size_t index,
                         float sample)
{
    const int16_t value = normalize_axis(device, sample);
    const int32_t delta = device->last_valid[index]
        ? (int32_t)value - device->last[index]
        : value;
    if (device->last_valid[index]) {
        if (value == device->last[index] ||
            (value != 0 && device->last[index] != 0 &&
             abs((int)delta) < ANALOG_JOYSTICK_MIN_CHANGE)) {
            return;
        }
    }
    const solar_os_input_axis_event_t event = {
        .axis = index == 0 ? SOLAR_OS_INPUT_AXIS_X : SOLAR_OS_INPUT_AXIS_Y,
        .value = value,
        .delta = delta,
    };
    if (solar_os_input_write_axis(device->input_source, &event) == ESP_OK) {
        device->last[index] = value;
        device->last_valid[index] = true;
    } else {
        device->dropped++;
    }
}

static void analog_joystick_worker(void *arg)
{
    solar_os_analog_joystick_device_t *device = arg;
    const solar_os_stream_read_options_t options = {
        .timeout_ms = 0,
    };
    while (!device->stop_requested) {
        for (size_t i = 0; i < 2; i++) {
            float sample = 0.0f;
            if (solar_os_stream_read_scalar(&device->streams[i],
                                            &options,
                                            &sample) == ESP_OK) {
                publish_axis(device, i, sample);
                device->samples++;
            } else {
                device->read_errors++;
            }
        }
        (void)ulTaskNotifyTake(pdTRUE,
                               pdMS_TO_TICKS(ANALOG_JOYSTICK_POLL_MS));
    }

    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static void clear_device(solar_os_analog_joystick_device_t *device)
{
    if (device == NULL) {
        return;
    }
    for (size_t i = 0; i < 2; i++) {
        solar_os_stream_close(&device->streams[i]);
    }
    if (device->input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(device->input_source);
    }
    memset(device, 0, sizeof(*device));
}

esp_err_t solar_os_analog_joystick_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (find_device(name) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_analog_joystick_device_t *device = alloc_device();
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(device, 0, sizeof(*device));
    device->active = true;
    device->streams[0] = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
    device->streams[1] = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
    strlcpy(device->name, name, sizeof(device->name));
    esp_err_t err = parse_bindings(bindings, binding_count, device);
    if (err != ESP_OK) {
        clear_device(device);
        return err;
    }

    err = solar_os_stream_open(device->x_stream, name, &device->streams[0]);
    if (err == ESP_OK) {
        err = solar_os_stream_open(device->y_stream, name, &device->streams[1]);
    }
    if (err == ESP_OK &&
        (device->streams[0].type != SOLAR_OS_STREAM_TYPE_SCALAR ||
         device->streams[1].type != SOLAR_OS_STREAM_TYPE_SCALAR)) {
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err == ESP_OK) {
        err = solar_os_input_joystick_source_open(name, &device->input_source);
    }
    if (err != ESP_OK) {
        clear_device(device);
        return err;
    }
    if (solar_os_task_create_pinned_internal(analog_joystick_worker,
                                             device->name,
                                             ANALOG_JOYSTICK_TASK_STACK,
                                             device,
                                             ANALOG_JOYSTICK_TASK_PRIORITY,
                                             &device->worker_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        clear_device(device);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "%s attached: x=%s y=%s range=%d..%d..%d deadzone=%d",
             name,
             device->x_stream,
             device->y_stream,
             device->minimum,
             device->center,
             device->maximum,
             device->deadzone);
    return ESP_OK;
}

esp_err_t solar_os_analog_joystick_detach(const char *name)
{
    solar_os_analog_joystick_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    device->stop_requested = true;
    if (device->worker_task != NULL) {
        (void)xTaskNotifyGive(device->worker_task);
    }
    if (!solar_os_task_wait_done(device->worker_task,
                                 &device->worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG,
             "%s detached: samples=%u dropped=%u read_errors=%u",
             name,
             (unsigned)device->samples,
             (unsigned)device->dropped,
             (unsigned)device->read_errors);
    clear_device(device);
    return ESP_OK;
}
