#include "solar_os_mpu6050.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mpu6050.h"
#include "driver/gpio.h"
#include "solar_os_buses.h"
#include "solar_os_imu.h"
#include "solar_os_stream.h"

#define MPU6050_DEVICE_MAX 2U
#define MPU6050_SAMPLE_CAPABILITIES \
    (SOLAR_OS_IMU_CAP_ACCELERATION | SOLAR_OS_IMU_CAP_ANGULAR_VELOCITY)

typedef struct {
    bool active;
    bool registered;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    int irq_pin;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    mpu6050_t chip;
} mpu6050_device_t;

static const char *TAG = "mpu6050";
static EXT_RAM_BSS_ATTR mpu6050_device_t devices[MPU6050_DEVICE_MAX];

static esp_err_t chip_read(void *context,
                           uint8_t reg,
                           uint8_t *data,
                           size_t len)
{
    mpu6050_device_t *device = context;
    return solar_os_bus_i2c_read_reg(device->i2c_bus, device->address,
                                     reg, data, len);
}

static esp_err_t chip_write(void *context,
                            uint8_t reg,
                            const uint8_t *data,
                            size_t len)
{
    mpu6050_device_t *device = context;
    return solar_os_bus_i2c_write_reg(device->i2c_bus, device->address,
                                      reg, data, len);
}

static esp_err_t read_sample(void *context,
                             uint32_t timeout_ms,
                             solar_os_imu_sample_t *sample)
{
    mpu6050_device_t *device = context;
    if (device == NULL || !device->active || sample == NULL ||
        timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t deadline =
        esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(device->mutex, ticks ? ticks : 1) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret = ESP_OK;
    bool ready = false;
    do {
        if (device->irq_pin < 0 || gpio_get_level(device->irq_pin))
            ret = mpu6050_data_ready(&device->chip, &ready);
        if (ret != ESP_OK || ready) break;
        if (esp_timer_get_time() >= deadline) {
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(1);
    } while (true);

    mpu6050_sample_t chip_sample;
    if (ret == ESP_OK) ret = mpu6050_read_sample(&device->chip, &chip_sample);
    if (ret == ESP_OK) {
        *sample = (solar_os_imu_sample_t) {
            .timestamp_us = (uint64_t)esp_timer_get_time(),
            .valid = MPU6050_SAMPLE_CAPABILITIES,
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

static const solar_os_imu_ops_t imu_ops = {
    .read_sample = read_sample,
};

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *address, int *irq_pin)
{
    bool have_i2c = false;
    bool have_address = false, have_irq = false;
    *irq_pin = -1;
    if (bindings == NULL || i2c_bus == NULL || address == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_i2c) {
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address &&
                   (binding->value == MPU6050_I2C_ADDRESS_LOW ||
                    binding->value == MPU6050_I2C_ADDRESS_HIGH)) {
            *address = (uint8_t)binding->value;
            have_address = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO && !have_irq &&
                   strcmp(binding->role, "irq") == 0 && GPIO_IS_VALID_GPIO(binding->value)) {
            *irq_pin = binding->value; have_irq = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_i2c && have_address &&
        solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL) ?
        ESP_OK : ESP_ERR_INVALID_ARG;
}

static void chip_delay(void *context, uint32_t ms)
{
    (void)context; TickType_t ticks = pdMS_TO_TICKS(ms); vTaskDelay(ticks ? ticks : 1);
}

static void clear_device(mpu6050_device_t *device)
{
    if (device == NULL) return;
    if (device->mutex != NULL) vSemaphoreDelete(device->mutex);
    memset(device, 0, sizeof(*device));
}

esp_err_t solar_os_mpu6050_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0' || strlen(name) >= SOLAR_OS_STREAM_DEVICE_MAX) return ESP_ERR_INVALID_ARG;
    mpu6050_device_t *device = NULL;
    for (size_t i = 0; i < MPU6050_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!devices[i].active && device == NULL) device = &devices[i];
    }
    if (device == NULL) return ESP_ERR_NO_MEM;

    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;
    int irq_pin = -1;
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       i2c_bus,
                                       sizeof(i2c_bus),
                                       &address, &irq_pin),
                        TAG,
                        "invalid bindings");

    memset(device, 0, sizeof(*device));
    device->active = true;
    device->address = address;
    device->irq_pin = irq_pin;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->i2c_bus, i2c_bus, sizeof(device->i2c_bus));
    device->mutex = xSemaphoreCreateMutexStatic(&device->mutex_storage);
    if (device->mutex == NULL) {
        clear_device(device);
        return ESP_ERR_NO_MEM;
    }

    if (irq_pin >= 0) {
        const gpio_config_t config = {.pin_bit_mask=1ULL << irq_pin, .mode=GPIO_MODE_INPUT,
            .pull_up_en=GPIO_PULLUP_DISABLE, .pull_down_en=GPIO_PULLDOWN_DISABLE, .intr_type=GPIO_INTR_DISABLE};
        esp_err_t ret = gpio_config(&config);
        if (ret != ESP_OK) { clear_device(device); return ret; }
    }
    const mpu6050_io_t io = {
        .read = chip_read,
        .write = chip_write,
        .delay_ms = chip_delay,
        .ctx = device,
    };
    esp_err_t ret = mpu6050_init(&device->chip, &io);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s chip initialization on %s addr=0x%02x failed: %s",
                 name, i2c_bus, address, esp_err_to_name(ret));
        clear_device(device);
        return ret;
    }

    const solar_os_imu_registration_t registration = {
        .name = name,
        .driver = "mpu6050",
        .capabilities = MPU6050_SAMPLE_CAPABILITIES,
        .ops = &imu_ops,
        .ctx = device,
    };
    ret = solar_os_imu_register(&registration);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s IMU registration failed: %s", name, esp_err_to_name(ret));
        (void)mpu6050_deinit(&device->chip);
        clear_device(device);
        return ret;
    }

    device->registered = true;
    ESP_LOGI(TAG,
             "%s attached on %s addr=0x%02x irq=%d",
             name,
             i2c_bus,
             address,
             irq_pin);
    return ESP_OK;
}

esp_err_t solar_os_mpu6050_detach(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < MPU6050_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            if (devices[i].registered) {
                ESP_RETURN_ON_ERROR(solar_os_imu_unregister(name), TAG, "unregister failed");
                devices[i].registered = false;
            }
            const esp_err_t ret = mpu6050_deinit(&devices[i].chip);
            if (ret == ESP_OK) clear_device(&devices[i]);
            return ret;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
