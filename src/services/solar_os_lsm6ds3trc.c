#include "solar_os_lsm6ds3trc.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lsm6ds3trc.h"
#include "solar_os_buses.h"
#include "solar_os_imu.h"
#include "solar_os_input.h"
#include "solar_os_task.h"

#define LSM6DS3TRC_DEVICE_MAX 2U
#define LSM6DS3TRC_POLL_MS 10U
#define LSM6DS3TRC_ORIENTATION_POLL_US 50000ULL
#define LSM6DS3TRC_TAP_WAIT_US 650000ULL
#define LSM6DS3TRC_SAMPLE_CAPABILITIES \
    (SOLAR_OS_IMU_CAP_ACCELERATION | SOLAR_OS_IMU_CAP_ANGULAR_VELOCITY)

typedef struct {
    bool active;
    bool registered;
    volatile bool stop_requested, worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    int irq_pin;
    uint8_t rotation, last_orientation;
    bool orientation_valid;
    uint64_t next_orientation_at;
    solar_os_input_source_t source;
    TaskHandle_t worker_task;
    uint8_t previous_events;
    bool tap_pending;
    uint64_t tap_at;
    uint32_t tap_raw;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    lsm6ds3trc_t chip;
} lsm6ds3trc_device_t;

static const char *TAG = "lsm6ds3trc";
static EXT_RAM_BSS_ATTR lsm6ds3trc_device_t devices[LSM6DS3TRC_DEVICE_MAX];

static void publish_event(lsm6ds3trc_device_t *device,
    solar_os_input_gesture_t gesture, uint32_t raw)
{
    const bool double_tap = gesture == SOLAR_OS_INPUT_GESTURE_DOUBLE_TAP;
    const solar_os_input_gesture_event_t event = {
        .gesture=gesture, .direction=SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE,
        .flags=double_tap ? SOLAR_OS_INPUT_GESTURE_FLAG_DOUBLE : 0,
        .value=double_tap ? 2 : 1, .raw=raw,
    };
    (void)solar_os_input_write_gesture(device->source, &event);
}

static void publish_orientation(lsm6ds3trc_device_t *device, uint8_t source, uint32_t raw)
{
    unsigned quarter;
    switch (source & 0x3fU) {
    case 0x08: quarter = 0; break;
    case 0x02: quarter = 1; break;
    case 0x04: quarter = 2; break;
    case 0x01: quarter = 3; break;
    default: return;
    }
    quarter = (quarter + device->rotation) & 3U;
    if (device->orientation_valid && device->last_orientation == quarter) return;
    static const solar_os_input_gesture_direction_t directions[] = {
        SOLAR_OS_INPUT_GESTURE_DIRECTION_NORTH, SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST,
        SOLAR_OS_INPUT_GESTURE_DIRECTION_SOUTH, SOLAR_OS_INPUT_GESTURE_DIRECTION_WEST,
    };
    const solar_os_input_gesture_event_t event = {
        .gesture=SOLAR_OS_INPUT_GESTURE_TILT, .direction=directions[quarter],
        .value=(int)(quarter * 90U), .raw=raw,
    };
    if (solar_os_input_write_gesture(device->source, &event) == ESP_OK) {
        device->last_orientation = quarter;
        device->orientation_valid = true;
    }
}

static void poll_motion_events(lsm6ds3trc_device_t *device)
{
    const uint64_t now = esp_timer_get_time();
    if (device->tap_pending && now - device->tap_at >= LSM6DS3TRC_TAP_WAIT_US) {
        publish_event(device, SOLAR_OS_INPUT_GESTURE_TAP, device->tap_raw);
        device->tap_pending = false;
    }
    if (!gpio_get_level(device->irq_pin)) {
        device->previous_events = 0;
        return;
    }
    lsm6ds3trc_events_t events;
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = lsm6ds3trc_read_events(&device->chip, &events);
    xSemaphoreGive(device->mutex);
    if (ret != ESP_OK) return;
    const unsigned fresh = events.flags & ~device->previous_events;
    device->previous_events = events.flags;
    const uint32_t raw = events.wake | ((uint32_t)events.tap << 8U) |
        ((uint32_t)events.function << 16U) | ((uint32_t)events.orientation << 24U);
    if (events.flags & LSM6DS3TRC_EVENT_ORIENTATION)
        publish_orientation(device, events.orientation, raw);
    if (fresh & LSM6DS3TRC_EVENT_DOUBLE_TAP) {
        device->tap_pending = false;
        publish_event(device, SOLAR_OS_INPUT_GESTURE_DOUBLE_TAP, raw);
    } else if (fresh & LSM6DS3TRC_EVENT_TAP) {
        device->tap_pending = true;
        device->tap_at = now;
        device->tap_raw = raw;
    }
    if (fresh & LSM6DS3TRC_EVENT_TILT) publish_event(device, SOLAR_OS_INPUT_GESTURE_TILT, raw);
    if (fresh & LSM6DS3TRC_EVENT_WAKE) publish_event(device, SOLAR_OS_INPUT_GESTURE_WAKE, raw);
    if (fresh & LSM6DS3TRC_EVENT_FREE_FALL) publish_event(device, SOLAR_OS_INPUT_GESTURE_FREE_FALL, raw);
}

static void poll_gestures(lsm6ds3trc_device_t *device)
{
    poll_motion_events(device);
    const uint64_t now = esp_timer_get_time();
    if (now < device->next_orientation_at) return;
    device->next_orientation_at = now + LSM6DS3TRC_ORIENTATION_POLL_US;
    uint8_t orientation;
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = lsm6ds3trc_read_orientation(&device->chip, &orientation);
    xSemaphoreGive(device->mutex);
    if (ret == ESP_OK) publish_orientation(device, orientation, (uint32_t)orientation << 24U);
}

static void gesture_worker(void *arg)
{
    lsm6ds3trc_device_t *device = arg;
    while (!device->stop_requested) {
        poll_gestures(device);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LSM6DS3TRC_POLL_MS));
    }
    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static bool stop_worker(lsm6ds3trc_device_t *device)
{
    if (!device->worker_task) return true;
    device->stop_requested = true;
    (void)xTaskNotifyGive(device->worker_task);
    if (!solar_os_task_wait_done(device->worker_task, &device->worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) return false;
    device->worker_task = NULL;
    return true;
}

static esp_err_t chip_read(void *context,
                           uint8_t reg,
                           uint8_t *data,
                           size_t len)
{
    lsm6ds3trc_device_t *device = context;
    return solar_os_bus_i2c_read_reg(device->i2c_bus, device->address,
                                     reg, data, len);
}

static esp_err_t chip_write(void *context,
                            uint8_t reg,
                            const uint8_t *data,
                            size_t len)
{
    lsm6ds3trc_device_t *device = context;
    return solar_os_bus_i2c_write_reg(device->i2c_bus, device->address,
                                      reg, data, len);
}

static esp_err_t read_sample(void *context,
                             uint32_t timeout_ms,
                             solar_os_imu_sample_t *sample)
{
    lsm6ds3trc_device_t *device = context;
    if (device == NULL || !device->active || sample == NULL ||
        timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const int64_t deadline =
        esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    esp_err_t ret = ESP_OK;
    bool ready = false;
    do {
        ret = lsm6ds3trc_data_ready(&device->chip, &ready);
        if (ret != ESP_OK || ready) break;
        if (esp_timer_get_time() >= deadline) {
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1U));
    } while (true);

    lsm6ds3trc_sample_t chip_sample;
    if (ret == ESP_OK) ret = lsm6ds3trc_read_sample(&device->chip, &chip_sample);
    if (ret == ESP_OK) {
        *sample = (solar_os_imu_sample_t) {
            .timestamp_us = (uint64_t)esp_timer_get_time(),
            .valid = LSM6DS3TRC_SAMPLE_CAPABILITIES,
        };
        memcpy(sample->acceleration_m_s2,
               chip_sample.acceleration_m_s2,
               sizeof(sample->acceleration_m_s2));
        memcpy(sample->angular_velocity_rad_s,
               chip_sample.angular_velocity_rad_s,
               sizeof(sample->angular_velocity_rad_s));
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static void chip_delay(void *ctx, uint32_t ms) {
    (void)ctx;
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static const solar_os_imu_ops_t imu_ops = {
    .read_sample = read_sample,
};

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *address, int *irq_pin, uint8_t *rotation)
{
    bool have_i2c = false;
    bool have_address = false;
    bool have_irq = false;
    bool have_rotation = false;
    if (bindings == NULL || i2c_bus == NULL || address == NULL || irq_pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *irq_pin = -1;
    *rotation = 0;
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_i2c) {
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address &&
                   (binding->value == LSM6DS3TRC_I2C_ADDRESS_LOW ||
                    binding->value == LSM6DS3TRC_I2C_ADDRESS_HIGH)) {
            *address = (uint8_t)binding->value;
            have_address = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO && !have_irq &&
                   strcmp(binding->role, "irq") == 0 && GPIO_IS_VALID_GPIO(binding->value)) {
            *irq_pin = binding->value;
            have_irq = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && !have_rotation &&
                   strcmp(binding->role, "rotation") == 0 &&
                   binding->value >= 0 && binding->value <= 3) {
            *rotation = (uint8_t)binding->value;
            have_rotation = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_i2c && have_address &&
        solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL) ?
        ESP_OK : ESP_ERR_INVALID_ARG;
}

static void clear_device(lsm6ds3trc_device_t *device)
{
    if (device == NULL) return;
    if (device->source) solar_os_input_source_close(device->source);
    if (device->mutex != NULL) vSemaphoreDelete(device->mutex);
    memset(device, 0, sizeof(*device));
}

esp_err_t solar_os_lsm6ds3trc_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0') return ESP_ERR_INVALID_ARG;
    lsm6ds3trc_device_t *device = NULL;
    for (size_t i = 0; i < LSM6DS3TRC_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!devices[i].active && device == NULL) device = &devices[i];
    }
    if (device == NULL) return ESP_ERR_NO_MEM;

    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;
    int irq_pin = -1;
    uint8_t rotation = 0;
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       i2c_bus,
                                       sizeof(i2c_bus),
                                       &address, &irq_pin, &rotation),
                        TAG,
                        "invalid bindings");

    memset(device, 0, sizeof(*device));
    device->active = true;
    device->address = address;
    device->irq_pin = irq_pin;
    device->rotation = rotation;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->i2c_bus, i2c_bus, sizeof(device->i2c_bus));
    device->mutex = xSemaphoreCreateMutexStatic(&device->mutex_storage);
    if (device->mutex == NULL) {
        clear_device(device);
        return ESP_ERR_NO_MEM;
    }

    const lsm6ds3trc_io_t io = {
        .read = chip_read,
        .write = chip_write,
        .ctx = device,
        .delay_ms = chip_delay,
    };
    esp_err_t ret = lsm6ds3trc_init(&device->chip, &io);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s chip initialization on %s addr=0x%02x failed: %s",
                 name, i2c_bus, address, esp_err_to_name(ret));
        clear_device(device);
        return ret;
    }

    if (irq_pin >= 0) {
        const gpio_config_t config = {
            .pin_bit_mask=1ULL << irq_pin, .mode=GPIO_MODE_INPUT,
            .pull_up_en=GPIO_PULLUP_DISABLE, .pull_down_en=GPIO_PULLDOWN_DISABLE,
            .intr_type=GPIO_INTR_DISABLE,
        };
        ret = gpio_config(&config);
        if (ret == ESP_OK) ret = lsm6ds3trc_enable_gestures(&device->chip);
        if (ret == ESP_OK) ret = solar_os_input_source_open_typed(name,
            SOLAR_OS_INPUT_SOURCE_GESTURE, SOLAR_OS_INPUT_CAP_GESTURE_EVENTS,
            true, &device->source);
        const uint32_t gestures = SOLAR_OS_INPUT_GESTURE_MASK(SOLAR_OS_INPUT_GESTURE_TAP) |
            SOLAR_OS_INPUT_GESTURE_MASK(SOLAR_OS_INPUT_GESTURE_DOUBLE_TAP) |
            SOLAR_OS_INPUT_GESTURE_MASK(SOLAR_OS_INPUT_GESTURE_TILT) |
            SOLAR_OS_INPUT_GESTURE_MASK(SOLAR_OS_INPUT_GESTURE_WAKE) |
            SOLAR_OS_INPUT_GESTURE_MASK(SOLAR_OS_INPUT_GESTURE_FREE_FALL);
        if (ret == ESP_OK) ret = solar_os_input_source_set_gestures(device->source, gestures);
        if (ret == ESP_OK && solar_os_task_create_pinned_internal(gesture_worker,
            name, 3072U, device, tskIDLE_PRIORITY + 1, &device->worker_task,
            tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) ret = ESP_ERR_NO_MEM;
        if (ret != ESP_OK) {
            (void)lsm6ds3trc_deinit(&device->chip);
            clear_device(device);
            return ret;
        }
    }

    const solar_os_imu_registration_t registration = {
        .name = name,
        .driver = "lsm6ds3tr-c",
        .capabilities = LSM6DS3TRC_SAMPLE_CAPABILITIES,
        .ops = &imu_ops,
        .ctx = device,
    };
    ret = solar_os_imu_register(&registration);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s IMU registration failed: %s", name, esp_err_to_name(ret));
        if (!stop_worker(device)) return ESP_ERR_TIMEOUT;
        (void)lsm6ds3trc_deinit(&device->chip);
        clear_device(device);
        return ret;
    }
    device->registered = true;

    ESP_LOGI(TAG,
             "%s attached on %s addr=0x%02x who=0x%02x",
             name,
             i2c_bus,
             address,
             device->chip.identity);
    return ESP_OK;
}

esp_err_t solar_os_lsm6ds3trc_detach(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < LSM6DS3TRC_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            if (devices[i].registered) ESP_RETURN_ON_ERROR(solar_os_imu_unregister(name),
                                TAG,
                                "unregister failed");
            devices[i].registered = false;
            if (!stop_worker(&devices[i])) return ESP_ERR_TIMEOUT;
            const esp_err_t ret = lsm6ds3trc_deinit(&devices[i].chip);
            clear_device(&devices[i]);
            return ret;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
