#include "solar_os_mgc3130.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mgc3130.h"
#include "solar_os_buses.h"
#include "solar_os_display.h"
#include "solar_os_input.h"
#include "solar_os_task.h"

#define MGC3130_POLL_MS 2U
#define MGC3130_TASK_STACK 4096U
#define MGC3130_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define MGC3130_RESET_LOW_MS 100U
#define MGC3130_BOOT_MS 500U
#define MGC3130_STATUS_TIMEOUT_MS 200U
#define MGC3130_MESSAGE_STATUS 0x15U
#define MGC3130_MESSAGE_SET_RUNTIME_PARAMETER 0xa2U
#define MGC3130_RUNTIME_GESTURES 0x0085U
#define MGC3130_RUNTIME_AIRWHEEL 0x0090U
#define MGC3130_RUNTIME_OUTPUT_ENABLE 0x00a0U
#define MGC3130_RUNTIME_OUTPUT_LOCK 0x00a1U
#define MGC3130_ALL_GESTURES_MASK 0xffc001ffUL
#define MGC3130_ALL_OUTPUT_MASK 0x0000001fUL
#define MGC3130_AIRWHEEL_ENABLE 0x00000020UL
#define MGC3130_TOUCH_MASK 0x001fUL
#define MGC3130_TAP_MASK 0x03e0UL
#define MGC3130_DOUBLE_TAP_MASK 0x7c00UL

typedef struct {
    bool active;
    bool pointer_valid;
    bool position_valid;
    bool airwheel_valid;
    volatile bool stop_requested;
    volatile bool worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    int transfer_pin;
    int reset_pin;
    uint8_t rotation;
    bool airwheel_enabled;
    uint16_t target_width;
    uint16_t target_height;
    int16_t pointer_x;
    int16_t pointer_y;
    int16_t axes[3];
    uint8_t last_gesture_code;
    uint32_t last_touch_info;
    uint8_t last_airwheel;
    uint8_t command_sequence;
    solar_os_input_source_t input_source;
    TaskHandle_t worker_task;
} solar_os_mgc3130_device_t;

static const char *TAG = "mgc3130";
static solar_os_mgc3130_device_t sensor;

static void mgc3130_worker(void *arg);

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                solar_os_mgc3130_device_t *device)
{
    bool have_i2c = false;
    bool have_address = false;
    bool have_transfer = false;
    bool have_reset = false;
    bool have_rotation = false;
    bool have_airwheel = false;
    if (bindings == NULL || device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    device->transfer_pin = -1;
    device->reset_pin = -1;
    device->airwheel_enabled = true;
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
            if (have_i2c) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(device->i2c_bus, binding->target, sizeof(device->i2c_bus));
            have_i2c = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
            if (have_address ||
                (binding->value != MGC3130_ADDRESS_PRIMARY &&
                 binding->value != MGC3130_ADDRESS_SECONDARY)) {
                return ESP_ERR_INVALID_ARG;
            }
            device->address = (uint8_t)binding->value;
            have_address = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_GPIO:
            if (strcmp(binding->role, "transfer") == 0 && !have_transfer) {
                device->transfer_pin = binding->value;
                have_transfer = true;
            } else if (strcmp(binding->role, "reset") == 0 && !have_reset) {
                device->reset_pin = binding->value;
                have_reset = true;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
            if (strcmp(binding->role, "rotation") == 0 && !have_rotation &&
                binding->value >= 0 && binding->value <= 3) {
                device->rotation = (uint8_t)binding->value;
                have_rotation = true;
            } else if (strcmp(binding->role, "airwheel") == 0 &&
                       !have_airwheel &&
                       (binding->value == 0 || binding->value == 1)) {
                device->airwheel_enabled = binding->value != 0;
                have_airwheel = true;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_i2c && have_address && have_transfer && have_reset &&
        have_rotation ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t read_frame(solar_os_mgc3130_device_t *device,
                            uint8_t frame[MGC3130_SENSOR_FRAME_SIZE])
{
    if (gpio_get_level(device->transfer_pin) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(gpio_set_level(device->transfer_pin, 0),
                        TAG,
                        "transfer assert failed");
    const esp_err_t ret = solar_os_bus_i2c_receive(device->i2c_bus,
                                                   device->address,
                                                   frame,
                                                   MGC3130_SENSOR_FRAME_SIZE);
    const esp_err_t release_ret = gpio_set_level(device->transfer_pin, 1);
    esp_rom_delay_us(200U);
    return ret != ESP_OK ? ret : release_ret;
}

static esp_err_t send_runtime_parameter(solar_os_mgc3130_device_t *device,
                                        uint16_t parameter,
                                        uint32_t value,
                                        uint32_t mask)
{
    uint8_t command[MGC3130_RUNTIME_FRAME_SIZE];
    ESP_RETURN_ON_ERROR(
        mgc3130_build_runtime_parameter(parameter,
                                        value,
                                        mask,
                                        device->command_sequence++,
                                        command),
        TAG,
        "runtime message build failed");
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_transmit(device->i2c_bus,
                                                  device->address,
                                                  command,
                                                  sizeof(command)),
                        TAG,
                        "runtime parameter 0x%04x write failed",
                        parameter);

    const TickType_t deadline = xTaskGetTickCount() +
        pdMS_TO_TICKS(MGC3130_STATUS_TIMEOUT_MS);
    do {
        uint8_t response[MGC3130_SENSOR_FRAME_SIZE] = {0};
        const esp_err_t read_err = read_frame(device, response);
        if (read_err == ESP_ERR_NOT_FOUND) {
            vTaskDelay(pdMS_TO_TICKS(1U));
            continue;
        }
        if (read_err != ESP_OK) {
            return read_err;
        }
        if (response[0] >= 8U && response[3] == MGC3130_MESSAGE_STATUS &&
            response[4] == MGC3130_MESSAGE_SET_RUNTIME_PARAMETER) {
            const uint16_t error =
                (uint16_t)((uint16_t)response[6] | ((uint16_t)response[7] << 8U));
            return error == 0U ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
    } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t configure_hardware(solar_os_mgc3130_device_t *device)
{
    const gpio_config_t reset_config = {
        .pin_bit_mask = 1ULL << (uint32_t)device->reset_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&reset_config), TAG, "reset config failed");
    const gpio_config_t transfer_config = {
        .pin_bit_mask = 1ULL << (uint32_t)device->transfer_pin,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&transfer_config),
                        TAG,
                        "transfer config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(device->transfer_pin, 1),
                        TAG,
                        "transfer release failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(device->reset_pin, 0),
                        TAG,
                        "reset low failed");
    vTaskDelay(pdMS_TO_TICKS(MGC3130_RESET_LOW_MS));
    ESP_RETURN_ON_ERROR(gpio_set_level(device->reset_pin, 1),
                        TAG,
                        "reset high failed");
    vTaskDelay(pdMS_TO_TICKS(MGC3130_BOOT_MS));
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_probe(device->i2c_bus, device->address),
                        TAG,
                        "MGC3130 not found");

    ESP_RETURN_ON_ERROR(send_runtime_parameter(device,
                                               MGC3130_RUNTIME_GESTURES,
                                               MGC3130_ALL_GESTURES_MASK,
                                               MGC3130_ALL_GESTURES_MASK),
                        TAG,
                        "gesture setup failed");
    ESP_RETURN_ON_ERROR(send_runtime_parameter(device,
                                               MGC3130_RUNTIME_AIRWHEEL,
                                               device->airwheel_enabled ?
                                                   MGC3130_AIRWHEEL_ENABLE : 0U,
                                               MGC3130_AIRWHEEL_ENABLE),
                        TAG,
                        "AirWheel setup failed");
    ESP_RETURN_ON_ERROR(send_runtime_parameter(device,
                                               MGC3130_RUNTIME_OUTPUT_ENABLE,
                                               MGC3130_ALL_OUTPUT_MASK,
                                               MGC3130_ALL_OUTPUT_MASK),
                        TAG,
                        "output setup failed");
    return send_runtime_parameter(device,
                                  MGC3130_RUNTIME_OUTPUT_LOCK,
                                  MGC3130_ALL_OUTPUT_MASK,
                                  MGC3130_ALL_OUTPUT_MASK);
}

static void publish_axis(solar_os_mgc3130_device_t *device,
                         solar_os_input_axis_t axis,
                         uint16_t raw,
                         bool had_position)
{
    const int16_t value = (int16_t)((int32_t)raw - 32768);
    const size_t index = (size_t)axis;
    if (had_position && value == device->axes[index]) {
        return;
    }
    const solar_os_input_axis_event_t event = {
        .axis = axis,
        .value = value,
        .delta = had_position ? (int32_t)value - device->axes[index] : 0,
    };
    if (solar_os_input_write_axis(device->input_source, &event) == ESP_OK) {
        device->axes[index] = value;
    }
}

static void map_pointer(const solar_os_mgc3130_device_t *device,
                        uint16_t raw_x,
                        uint16_t raw_y,
                        int16_t *x,
                        int16_t *y)
{
    const uint16_t native_width = (device->rotation & 1U) != 0U ?
        device->target_height : device->target_width;
    const uint16_t native_height = (device->rotation & 1U) != 0U ?
        device->target_width : device->target_height;
    const uint16_t sx = (uint16_t)(((uint32_t)raw_x * (native_width - 1U) +
                                    32767U) / 65535U);
    const uint16_t sy = (uint16_t)(((uint32_t)(65535U - raw_y) *
                                    (native_height - 1U) + 32767U) / 65535U);
    switch (device->rotation) {
    case 0:
        *x = (int16_t)sx;
        *y = (int16_t)sy;
        break;
    case 1:
        *x = (int16_t)sy;
        *y = (int16_t)(native_width - 1U - sx);
        break;
    case 2:
        *x = (int16_t)(native_width - 1U - sx);
        *y = (int16_t)(native_height - 1U - sy);
        break;
    default:
        *x = (int16_t)(native_height - 1U - sy);
        *y = (int16_t)sx;
        break;
    }
}

static void publish_pointer(solar_os_mgc3130_device_t *device,
                            const mgc3130_sample_t *sample)
{
    const bool position_valid = sample->has_position &&
        (sample->system_info & MGC3130_SYSTEM_POSITION_VALID) != 0U;
    if (!position_valid) {
        device->pointer_valid = false;
        return;
    }
    int16_t x;
    int16_t y;
    map_pointer(device, sample->x, sample->y, &x, &y);
    if (device->pointer_valid && x == device->pointer_x &&
        y == device->pointer_y) {
        return;
    }

    solar_os_input_pointer_event_t event = {
        .pointer_id = 0,
        .buttons = 0,
        .mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
        .action = SOLAR_OS_INPUT_POINTER_MOVE,
        .x = x,
        .y = y,
        .delta_x = (int16_t)(x - device->pointer_x),
        .delta_y = (int16_t)(y - device->pointer_y),
    };
    strlcpy(event.target, SOLAR_OS_DISPLAY_PRIMARY_TARGET, sizeof(event.target));
    if (solar_os_input_write_pointer(device->input_source, &event) == ESP_OK) {
        device->pointer_valid = true;
        device->pointer_x = x;
        device->pointer_y = y;
    }
}

static uint32_t gesture_flags(uint32_t gesture_info)
{
    uint32_t flags = 0U;
    if ((gesture_info & (1UL << 16)) != 0U) {
        flags |= SOLAR_OS_INPUT_GESTURE_FLAG_EDGE;
    }
    if ((gesture_info & MGC3130_GESTURE_HAND_PRESENT) != 0U) {
        flags |= SOLAR_OS_INPUT_GESTURE_FLAG_HAND_PRESENT;
    }
    if ((gesture_info & MGC3130_GESTURE_HAND_HELD) != 0U) {
        flags |= SOLAR_OS_INPUT_GESTURE_FLAG_HAND_HELD;
    }
    if ((gesture_info & MGC3130_GESTURE_HAND_INSIDE) != 0U) {
        flags |= SOLAR_OS_INPUT_GESTURE_FLAG_HAND_INSIDE;
    }
    if ((gesture_info & MGC3130_GESTURE_IN_PROGRESS) != 0U) {
        flags |= SOLAR_OS_INPUT_GESTURE_FLAG_IN_PROGRESS;
    }
    return flags;
}

static bool decode_gesture(uint8_t code,
                           solar_os_input_gesture_event_t *event)
{
    memset(event, 0, sizeof(*event));
    switch (code) {
    case 2:
    case 65:
    case 69:
        event->gesture = SOLAR_OS_INPUT_GESTURE_FLICK;
        event->direction = SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST;
        break;
    case 3:
    case 66:
    case 70:
        event->gesture = SOLAR_OS_INPUT_GESTURE_FLICK;
        event->direction = SOLAR_OS_INPUT_GESTURE_DIRECTION_WEST;
        break;
    case 4:
    case 67:
    case 71:
        event->gesture = SOLAR_OS_INPUT_GESTURE_FLICK;
        event->direction = SOLAR_OS_INPUT_GESTURE_DIRECTION_NORTH;
        break;
    case 5:
    case 68:
    case 72:
        event->gesture = SOLAR_OS_INPUT_GESTURE_FLICK;
        event->direction = SOLAR_OS_INPUT_GESTURE_DIRECTION_SOUTH;
        break;
    case 6:
        event->gesture = SOLAR_OS_INPUT_GESTURE_CIRCLE;
        event->direction = SOLAR_OS_INPUT_GESTURE_DIRECTION_CLOCKWISE;
        break;
    case 7:
        event->gesture = SOLAR_OS_INPUT_GESTURE_CIRCLE;
        event->direction = SOLAR_OS_INPUT_GESTURE_DIRECTION_COUNTERCLOCKWISE;
        break;
    case 8:
        event->gesture = SOLAR_OS_INPUT_GESTURE_WAVE;
        event->direction = SOLAR_OS_INPUT_GESTURE_DIRECTION_HORIZONTAL;
        break;
    case 9:
        event->gesture = SOLAR_OS_INPUT_GESTURE_WAVE;
        event->direction = SOLAR_OS_INPUT_GESTURE_DIRECTION_VERTICAL;
        break;
    case 64:
        event->gesture = SOLAR_OS_INPUT_GESTURE_HOLD;
        event->direction = SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE;
        break;
    case 73:
        event->gesture = SOLAR_OS_INPUT_GESTURE_PRESENCE;
        event->direction = SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE;
        break;
    default:
        return false;
    }
    if (code >= 65U && code <= 68U) {
        event->flags |= SOLAR_OS_INPUT_GESTURE_FLAG_EDGE;
    } else if (code >= 69U && code <= 72U) {
        event->flags |= SOLAR_OS_INPUT_GESTURE_FLAG_DOUBLE;
    }
    return true;
}

static solar_os_input_gesture_direction_t touch_direction(unsigned index)
{
    static const solar_os_input_gesture_direction_t directions[] = {
        SOLAR_OS_INPUT_GESTURE_DIRECTION_SOUTH,
        SOLAR_OS_INPUT_GESTURE_DIRECTION_WEST,
        SOLAR_OS_INPUT_GESTURE_DIRECTION_NORTH,
        SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST,
        SOLAR_OS_INPUT_GESTURE_DIRECTION_CENTER,
    };
    return index < sizeof(directions) / sizeof(directions[0]) ?
        directions[index] : SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE;
}

static void publish_gestures(solar_os_mgc3130_device_t *device,
                             const mgc3130_sample_t *sample)
{
    const uint32_t info = sample->has_gesture ? sample->gesture_info : 0U;
    const uint8_t code = (uint8_t)info;
    if (code != 0U && code != device->last_gesture_code) {
        solar_os_input_gesture_event_t event;
        if (decode_gesture(code, &event)) {
            event.flags |= gesture_flags(info);
            event.raw = info;
            (void)solar_os_input_write_gesture(device->input_source, &event);
        }
    }
    device->last_gesture_code = code;

    const uint32_t touch = sample->has_touch ? sample->touch_info : 0U;
    const uint32_t rising = touch & ~device->last_touch_info;
    for (unsigned i = 0; i < 5U; i++) {
        solar_os_input_gesture_event_t event = {
            .direction = touch_direction(i),
            .raw = touch,
        };
        if ((rising & (1UL << (5U + i))) != 0U) {
            event.gesture = SOLAR_OS_INPUT_GESTURE_TAP;
            (void)solar_os_input_write_gesture(device->input_source, &event);
        }
        if ((rising & (1UL << (10U + i))) != 0U) {
            event.gesture = SOLAR_OS_INPUT_GESTURE_DOUBLE_TAP;
            event.flags = SOLAR_OS_INPUT_GESTURE_FLAG_DOUBLE;
            (void)solar_os_input_write_gesture(device->input_source, &event);
        }
    }
    device->last_touch_info = touch &
        (MGC3130_TOUCH_MASK | MGC3130_TAP_MASK | MGC3130_DOUBLE_TAP_MASK);

    const bool airwheel_valid = sample->has_airwheel &&
        (sample->system_info & MGC3130_SYSTEM_AIRWHEEL_VALID) != 0U;
    if (airwheel_valid && device->airwheel_valid) {
        const int8_t delta = (int8_t)(sample->airwheel - device->last_airwheel);
        if (delta != 0) {
            const solar_os_input_gesture_event_t event = {
                .gesture = SOLAR_OS_INPUT_GESTURE_AIRWHEEL,
                .direction = delta > 0 ?
                    SOLAR_OS_INPUT_GESTURE_DIRECTION_CLOCKWISE :
                    SOLAR_OS_INPUT_GESTURE_DIRECTION_COUNTERCLOCKWISE,
                .flags = gesture_flags(info),
                .value = delta,
                .raw = sample->airwheel,
            };
            (void)solar_os_input_write_gesture(device->input_source, &event);
        }
    }
    device->airwheel_valid = airwheel_valid;
    if (airwheel_valid) {
        device->last_airwheel = sample->airwheel;
    }
}

static void process_sample(solar_os_mgc3130_device_t *device,
                           const mgc3130_sample_t *sample)
{
    const bool position_valid = sample->has_position &&
        (sample->system_info & MGC3130_SYSTEM_POSITION_VALID) != 0U;
    if (position_valid) {
        publish_axis(device, SOLAR_OS_INPUT_AXIS_X, sample->x, device->position_valid);
        publish_axis(device, SOLAR_OS_INPUT_AXIS_Y, sample->y, device->position_valid);
        publish_axis(device, SOLAR_OS_INPUT_AXIS_Z, sample->z, device->position_valid);
    }
    publish_pointer(device, sample);
    publish_gestures(device, sample);
    device->position_valid = position_valid;
}

static void clear_device(void)
{
    if (sensor.input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(sensor.input_source);
    }
    memset(&sensor, 0, sizeof(sensor));
}

esp_err_t solar_os_mgc3130_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0' ||
        strlen(name) >= SOLAR_OS_INPUT_SOURCE_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sensor.active) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_mgc3130_device_t candidate = {0};
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &candidate),
                        TAG,
                        "invalid bindings");
    solar_os_display_target_t target;
    if (!solar_os_display_find_target(SOLAR_OS_DISPLAY_PRIMARY_TARGET, &target) ||
        target.width == 0U || target.height == 0U ||
        target.width > INT16_MAX || target.height > INT16_MAX) {
        return ESP_ERR_NOT_FOUND;
    }
    candidate.target_width = target.width;
    candidate.target_height = target.height;
    ESP_RETURN_ON_ERROR(configure_hardware(&candidate),
                        TAG,
                        "controller init failed");

    strlcpy(candidate.name, name, sizeof(candidate.name));
    const uint32_t capabilities = SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE |
        SOLAR_OS_INPUT_CAP_AXIS_EVENTS |
        SOLAR_OS_INPUT_CAP_GESTURE_EVENTS;
    esp_err_t ret = solar_os_input_source_open_typed(candidate.name,
                                                     SOLAR_OS_INPUT_SOURCE_GESTURE,
                                                     capabilities,
                                                     true,
                                                     &candidate.input_source);
    if (ret != ESP_OK) {
        return ret;
    }
    candidate.active = true;
    sensor = candidate;
    if (solar_os_task_create_pinned_internal(mgc3130_worker,
                                             sensor.name,
                                             MGC3130_TASK_STACK,
                                             &sensor,
                                             MGC3130_TASK_PRIORITY,
                                             &sensor.worker_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        clear_device();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "%s attached on %s addr=0x%02x transfer=%d reset=%d rotation=%u airwheel=%s",
             sensor.name,
             sensor.i2c_bus,
             sensor.address,
             sensor.transfer_pin,
             sensor.reset_pin,
             sensor.rotation,
             sensor.airwheel_enabled ? "on" : "off");
    return ESP_OK;
}

esp_err_t solar_os_mgc3130_detach(const char *name)
{
    if (!sensor.active || name == NULL || strcmp(sensor.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    sensor.stop_requested = true;
    (void)xTaskNotifyGive(sensor.worker_task);
    if (!solar_os_task_wait_done(sensor.worker_task,
                                 &sensor.worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    clear_device();
    return ESP_OK;
}

static void mgc3130_worker(void *arg)
{
    solar_os_mgc3130_device_t *device = arg;
    while (!device->stop_requested) {
        uint8_t frame[MGC3130_SENSOR_FRAME_SIZE] = {0};
        const esp_err_t read_err = read_frame(device, frame);
        if (read_err == ESP_OK) {
            mgc3130_sample_t sample;
            if (mgc3130_parse_sensor_frame(frame, sizeof(frame), &sample) == ESP_OK) {
                process_sample(device, &sample);
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MGC3130_POLL_MS));
    }
    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}
