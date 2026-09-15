#include "solar_os_bhi260ap.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bhi260ap_codec.h"
#include "bhy2.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_imu.h"

#define BHI260AP_DEVICE_MAX 2U
#define BHI260AP_I2C_TRANSFER_MAX 256U
#define BHI260AP_FIFO_BUFFER_SIZE 512U
#define BHI260AP_SAMPLE_RATE_HZ 100.0F
#define BHI260AP_SAMPLE_CAPABILITIES \
    (SOLAR_OS_IMU_CAP_ACCELERATION | \
     SOLAR_OS_IMU_CAP_ANGULAR_VELOCITY | \
     SOLAR_OS_IMU_CAP_ORIENTATION)

extern const uint8_t bhi260ap_firmware_start[]
    asm("_binary_BHI260AP_fw_start");
extern const uint8_t bhi260ap_firmware_end[]
    asm("_binary_BHI260AP_fw_end");

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    int irq_pin;
    esp_err_t last_bus_error;
    struct bhy2_dev bhy2;
    uint8_t fifo_buffer[BHI260AP_FIFO_BUFFER_SIZE];
    solar_os_imu_sample_t latest;
    solar_os_imu_capabilities_t fresh;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
} bhi260ap_device_t;

static const char *TAG = "bhi260ap";
static EXT_RAM_BSS_ATTR bhi260ap_device_t devices[BHI260AP_DEVICE_MAX];

static esp_err_t bhy2_error(bhi260ap_device_t *device, int8_t result)
{
    if (result == BHY2_OK) {
        return ESP_OK;
    }
    if (result == BHY2_E_IO && device->last_bus_error != ESP_OK) {
        return device->last_bus_error;
    }
    if (result == BHY2_E_TIMEOUT) {
        return ESP_ERR_TIMEOUT;
    }
    if (result == BHY2_E_NULL_PTR || result == BHY2_E_INVALID_PARAM) {
        return ESP_ERR_INVALID_ARG;
    }
    if (result == BHY2_E_BUFFER) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static int8_t i2c_read(uint8_t reg_addr,
                       uint8_t *reg_data,
                       uint32_t length,
                       void *intf_ptr)
{
    bhi260ap_device_t *device = intf_ptr;
    device->last_bus_error = solar_os_bus_i2c_read_reg(
        device->i2c_bus, device->address, reg_addr, reg_data, length);
    return device->last_bus_error == ESP_OK ? BHY2_INTF_RET_SUCCESS : -1;
}

static int8_t i2c_write(uint8_t reg_addr,
                        const uint8_t *reg_data,
                        uint32_t length,
                        void *intf_ptr)
{
    bhi260ap_device_t *device = intf_ptr;
    device->last_bus_error = solar_os_bus_i2c_write_reg(
        device->i2c_bus, device->address, reg_addr, reg_data, length);
    return device->last_bus_error == ESP_OK ? BHY2_INTF_RET_SUCCESS : -1;
}

static void delay_us(uint32_t period_us, void *intf_ptr)
{
    (void)intf_ptr;
    if (period_us >= 1000U) {
        vTaskDelay(pdMS_TO_TICKS((period_us + 999U) / 1000U));
    } else {
        esp_rom_delay_us(period_us);
    }
}

static uint64_t callback_timestamp_us(
    const struct bhy2_fifo_parse_data_info *info)
{
    return info->time_stamp != NULL ?
        bhi260ap_timestamp_us(*info->time_stamp) : 0U;
}

static void update_timestamp(
    bhi260ap_device_t *device,
    const struct bhy2_fifo_parse_data_info *info)
{
    const uint64_t timestamp_us = callback_timestamp_us(info);
    if (timestamp_us > device->latest.timestamp_us) {
        device->latest.timestamp_us = timestamp_us;
    }
}

static void acceleration_callback(
    const struct bhy2_fifo_parse_data_info *info,
    void *callback_ref)
{
    bhi260ap_device_t *device = callback_ref;
    if (info->data_size != 7U) {
        return;
    }
    bhi260ap_decode_acceleration(info->data_ptr,
                                 device->latest.acceleration_m_s2);
    update_timestamp(device, info);
    device->fresh |= SOLAR_OS_IMU_CAP_ACCELERATION;
}

static void angular_velocity_callback(
    const struct bhy2_fifo_parse_data_info *info,
    void *callback_ref)
{
    bhi260ap_device_t *device = callback_ref;
    if (info->data_size != 7U) {
        return;
    }
    bhi260ap_decode_angular_velocity(
        info->data_ptr, device->latest.angular_velocity_rad_s);
    update_timestamp(device, info);
    device->fresh |= SOLAR_OS_IMU_CAP_ANGULAR_VELOCITY;
}

static void orientation_callback(
    const struct bhy2_fifo_parse_data_info *info,
    void *callback_ref)
{
    bhi260ap_device_t *device = callback_ref;
    if (info->data_size != 11U) {
        return;
    }
    bhi260ap_decode_quaternion(info->data_ptr, device->latest.orientation);
    update_timestamp(device, info);
    device->fresh |= SOLAR_OS_IMU_CAP_ORIENTATION;
}

static esp_err_t configure_sensor(bhi260ap_device_t *device,
                                  uint8_t sensor_id,
                                  bhy2_fifo_parse_callback_t callback)
{
    ESP_RETURN_ON_ERROR(
        bhy2_error(device, bhy2_register_fifo_parse_callback(
            sensor_id, callback, device, &device->bhy2)),
        TAG,
        "FIFO callback registration failed for sensor %u",
        sensor_id);
    return bhy2_error(device, bhy2_set_virt_sensor_cfg(
        sensor_id, BHI260AP_SAMPLE_RATE_HZ, 0U, &device->bhy2));
}

static esp_err_t initialize_chip(bhi260ap_device_t *device)
{
    int8_t result = bhy2_init(BHY2_I2C_INTERFACE,
                              i2c_read,
                              i2c_write,
                              delay_us,
                              BHI260AP_I2C_TRANSFER_MAX,
                              device,
                              &device->bhy2);
    ESP_RETURN_ON_ERROR(bhy2_error(device, result), TAG, "BHy2 init failed");
    ESP_RETURN_ON_ERROR(bhy2_error(device, bhy2_soft_reset(&device->bhy2)),
                        TAG,
                        "soft reset failed");

    uint8_t product_id = 0U;
    ESP_RETURN_ON_ERROR(
        bhy2_error(device, bhy2_get_product_id(&product_id, &device->bhy2)),
        TAG,
        "product ID read failed");
    if (product_id != BHY2_PRODUCT_ID) {
        ESP_LOGE(TAG, "unexpected product ID 0x%02x", product_id);
        return ESP_ERR_NOT_FOUND;
    }

    const uint8_t interrupt_control =
        BHY2_ICTL_DISABLE_STATUS_FIFO | BHY2_ICTL_DISABLE_DEBUG;
    ESP_RETURN_ON_ERROR(
        bhy2_error(device, bhy2_set_host_interrupt_ctrl(
            interrupt_control, &device->bhy2)),
        TAG,
        "interrupt configuration failed");
    ESP_RETURN_ON_ERROR(
        bhy2_error(device, bhy2_set_host_intf_ctrl(0U, &device->bhy2)),
        TAG,
        "host interface configuration failed");

    uint8_t boot_status = 0U;
    ESP_RETURN_ON_ERROR(
        bhy2_error(device, bhy2_get_boot_status(&boot_status, &device->bhy2)),
        TAG,
        "boot status read failed");
    if ((boot_status & BHY2_BST_HOST_INTERFACE_READY) == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t firmware_size =
        (uint32_t)(bhi260ap_firmware_end - bhi260ap_firmware_start);
    ESP_LOGI(TAG, "uploading %" PRIu32 " bytes of RAM firmware", firmware_size);
    ESP_RETURN_ON_ERROR(
        bhy2_error(device, bhy2_upload_firmware_to_ram(
            bhi260ap_firmware_start, firmware_size, &device->bhy2)),
        TAG,
        "RAM firmware upload failed");
    ESP_RETURN_ON_ERROR(
        bhy2_error(device, bhy2_boot_from_ram(&device->bhy2)),
        TAG,
        "RAM firmware boot failed");

    uint16_t kernel_version = 0U;
    ESP_RETURN_ON_ERROR(
        bhy2_error(device, bhy2_get_kernel_version(
            &kernel_version, &device->bhy2)),
        TAG,
        "kernel version read failed");
    if (kernel_version == 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_RETURN_ON_ERROR(
        bhy2_error(device, bhy2_update_virtual_sensor_list(&device->bhy2)),
        TAG,
        "virtual sensor discovery failed");
    ESP_RETURN_ON_ERROR(configure_sensor(device,
                                         BHY2_SENSOR_ID_ACC_PASS,
                                         acceleration_callback),
                        TAG,
                        "accelerometer setup failed");
    ESP_RETURN_ON_ERROR(configure_sensor(device,
                                         BHY2_SENSOR_ID_GYRO_PASS,
                                         angular_velocity_callback),
                        TAG,
                        "gyroscope setup failed");
    ESP_RETURN_ON_ERROR(configure_sensor(device,
                                         BHY2_SENSOR_ID_GAMERV,
                                         orientation_callback),
                        TAG,
                        "orientation setup failed");

    ESP_LOGI(TAG, "firmware booted, kernel version %u", kernel_version);
    return ESP_OK;
}

static esp_err_t read_sample(void *ctx,
                             uint32_t timeout_ms,
                             solar_os_imu_sample_t *sample)
{
    bhi260ap_device_t *device = ctx;
    if (device == NULL || !device->active || sample == NULL ||
        timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    device->fresh = 0U;
    const int64_t deadline =
        esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    esp_err_t ret = ESP_OK;
    while (device->fresh != BHI260AP_SAMPLE_CAPABILITIES) {
        ret = bhy2_error(device, bhy2_get_and_process_fifo(
            device->fifo_buffer,
            sizeof(device->fifo_buffer),
            &device->bhy2));
        if (ret != ESP_OK) {
            break;
        }
        if (esp_timer_get_time() >= deadline) {
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
    if (ret == ESP_OK) {
        device->latest.valid = device->fresh;
        *sample = device->latest;
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
                                uint8_t *address,
                                int *irq_pin)
{
    bool have_i2c = false;
    bool have_address = false;
    bool have_irq = false;
    if (bindings == NULL || i2c_bus == NULL || address == NULL ||
        irq_pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_i2c) {
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address &&
                   (binding->value == 0x28 || binding->value == 0x29)) {
            *address = (uint8_t)binding->value;
            have_address = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   strcmp(binding->role, "irq") == 0 && !have_irq) {
            *irq_pin = binding->value;
            have_irq = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_i2c && have_address && have_irq &&
        solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL) ?
        ESP_OK : ESP_ERR_INVALID_ARG;
}

static void clear_device(bhi260ap_device_t *device)
{
    if (device == NULL) {
        return;
    }
    const int irq_pin = device->irq_pin;
    if (device->mutex != NULL) {
        vSemaphoreDelete(device->mutex);
    }
    memset(device, 0, sizeof(*device));
    device->irq_pin = -1;
    if (irq_pin >= 0) {
        (void)gpio_reset_pin((gpio_num_t)irq_pin);
    }
}

esp_err_t solar_os_bhi260ap_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    bhi260ap_device_t *device = NULL;
    for (size_t i = 0; i < BHI260AP_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!devices[i].active && device == NULL) {
            device = &devices[i];
        }
    }
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;
    int irq_pin = -1;
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       i2c_bus,
                                       sizeof(i2c_bus),
                                       &address,
                                       &irq_pin),
                        TAG,
                        "invalid bindings");
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_probe(i2c_bus, address),
                        TAG,
                        "BHI260AP not found");

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

    const gpio_config_t irq_config = {
        .pin_bit_mask = 1ULL << (uint32_t)irq_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&irq_config);
    if (ret == ESP_OK) {
        ret = initialize_chip(device);
    }
    if (ret != ESP_OK) {
        clear_device(device);
        return ret;
    }

    const solar_os_imu_registration_t registration = {
        .name = name,
        .driver = "bhi260ap",
        .capabilities = BHI260AP_SAMPLE_CAPABILITIES,
        .ops = &imu_ops,
        .ctx = device,
    };
    ret = solar_os_imu_register(&registration);
    if (ret != ESP_OK) {
        clear_device(device);
        return ret;
    }
    ESP_LOGI(TAG,
             "%s attached on %s addr=0x%02x irq=%d",
             name,
             i2c_bus,
             address,
             irq_pin);
    return ESP_OK;
}

esp_err_t solar_os_bhi260ap_detach(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < BHI260AP_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            ESP_RETURN_ON_ERROR(solar_os_imu_unregister(name),
                                TAG,
                                "unregister failed");
            clear_device(&devices[i]);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
