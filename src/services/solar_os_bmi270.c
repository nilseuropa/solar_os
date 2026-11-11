#include "solar_os_bmi270.h"

#include <stdbool.h>
#include <string.h>

#include "bmi270.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_buses.h"
#include "solar_os_imu.h"

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    uint8_t rotation;
    int irq_pin;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    bmi270_t chip;
} solar_os_bmi270_device_t;

static const char *TAG = "bmi270";
static solar_os_bmi270_device_t imu_device;

static esp_err_t chip_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    solar_os_bmi270_device_t *device = ctx;
    return solar_os_bus_i2c_read_reg(device->i2c_bus, device->address, reg, data, len);
}

static esp_err_t chip_write(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    solar_os_bmi270_device_t *device = ctx;
    return solar_os_bus_i2c_write_reg(device->i2c_bus, device->address, reg, data, len);
}

static void rotate_axes(uint8_t rotation, float *x, float *y)
{
    const float in_x = *x;
    const float in_y = *y;
    switch (rotation & 3U) {
    case 1U:
        *x = in_y;
        *y = -in_x;
        break;
    case 2U:
        *x = -in_x;
        *y = -in_y;
        break;
    case 3U:
        *x = -in_y;
        *y = in_x;
        break;
    default:
        break;
    }
}

static esp_err_t read_sample(void *ctx, uint32_t timeout_ms, solar_os_imu_sample_t *sample)
{
    (void)timeout_ms;
    solar_os_bmi270_device_t *device = ctx;
    if (device == NULL || !device->active || sample == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    bmi270_sample_t raw;
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = bmi270_read_sample(&device->chip, &raw);
    xSemaphoreGive(device->mutex);
    if (ret != ESP_OK) {
        return ret;
    }
    memset(sample, 0, sizeof(*sample));
    sample->timestamp_us = (uint64_t)esp_timer_get_time();
    rotate_axes(device->rotation, &raw.accel_m_s2[0], &raw.accel_m_s2[1]);
    rotate_axes(device->rotation, &raw.gyro_rad_s[0], &raw.gyro_rad_s[1]);
    memcpy(sample->acceleration_m_s2, raw.accel_m_s2, sizeof(sample->acceleration_m_s2));
    memcpy(sample->angular_velocity_rad_s, raw.gyro_rad_s,
           sizeof(sample->angular_velocity_rad_s));
    if (raw.accel_valid) {
        sample->valid |= SOLAR_OS_IMU_CAP_ACCELERATION;
    }
    if (raw.gyro_valid) {
        sample->valid |= SOLAR_OS_IMU_CAP_ANGULAR_VELOCITY;
    }
    return ESP_OK;
}

static const solar_os_imu_ops_t imu_ops = {
    .read_sample = read_sample,
};

esp_err_t solar_os_bmi270_attach(const char *name,
                                 const solar_os_expansion_binding_t *bindings,
                                 size_t binding_count)
{
    if (name == NULL || name[0] == '\0' || imu_device.active) {
        return ESP_ERR_INVALID_ARG;
    }
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;
    uint8_t rotation = 0U;
    int irq_pin = -1;
    bool have_i2c = false;
    bool have_address = false;
    for (size_t i = 0; bindings != NULL && i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_i2c) {
            strlcpy(i2c_bus, binding->target, sizeof(i2c_bus));
            have_i2c = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address) {
            address = (uint8_t)binding->value;
            have_address = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   strcmp(binding->role, "irq") == 0) {
            irq_pin = binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "rotation") == 0) {
            rotation = (uint8_t)binding->value;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (!have_i2c || !have_address || rotation > 3U ||
        !solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_probe(i2c_bus, address),
                        TAG, "BMI270 not found");
    memset(&imu_device, 0, sizeof(imu_device));
    imu_device.address = address;
    imu_device.rotation = rotation;
    imu_device.irq_pin = irq_pin;
    strlcpy(imu_device.name, name, sizeof(imu_device.name));
    strlcpy(imu_device.i2c_bus, i2c_bus, sizeof(imu_device.i2c_bus));
    imu_device.mutex = xSemaphoreCreateMutexStatic(&imu_device.mutex_storage);
    if (imu_device.mutex == NULL) {
        memset(&imu_device, 0, sizeof(imu_device));
        return ESP_ERR_NO_MEM;
    }
    const bmi270_io_t io = {
        .read = chip_read,
        .write = chip_write,
        .ctx = &imu_device,
    };
    esp_err_t ret = bmi270_init(&imu_device.chip, &io);
    if (ret != ESP_OK) {
        memset(&imu_device, 0, sizeof(imu_device));
        return ret;
    }
    imu_device.active = true;
    const solar_os_imu_registration_t registration = {
        .name = name,
        .driver = "bmi270",
        .capabilities = SOLAR_OS_IMU_CAP_ACCELERATION | SOLAR_OS_IMU_CAP_ANGULAR_VELOCITY,
        .ops = &imu_ops,
        .ctx = &imu_device,
    };
    ret = solar_os_imu_register(&registration);
    if (ret != ESP_OK) {
        (void)bmi270_deinit(&imu_device.chip);
        memset(&imu_device, 0, sizeof(imu_device));
        return ret;
    }
    ESP_LOGI(TAG, "%s attached on %s address 0x%02x", name, i2c_bus, address);
    return ESP_OK;
}

esp_err_t solar_os_bmi270_detach(const char *name)
{
    if (!imu_device.active || name == NULL || strcmp(imu_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(solar_os_imu_unregister(name), TAG, "unregister failed");
    (void)bmi270_deinit(&imu_device.chip);
    memset(&imu_device, 0, sizeof(imu_device));
    return ESP_OK;
}
