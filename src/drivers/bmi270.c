#include "bmi270.h"

#include "bmi270_config.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REG_CHIP_ID 0x00U
#define REG_DATA_8 0x0CU
#define REG_INTERNAL_STATUS 0x21U
#define REG_ACC_CONF 0x40U
#define REG_ACC_RANGE 0x41U
#define REG_GYR_CONF 0x42U
#define REG_GYR_RANGE 0x43U
#define REG_INIT_CTRL 0x59U
#define REG_INIT_ADDR_0 0x5BU
#define REG_INIT_DATA 0x5EU
#define REG_PWR_CONF 0x7CU
#define REG_PWR_CTRL 0x7DU
#define REG_CMD 0x7EU
#define CMD_SOFTRESET 0xB6U
#define CHUNK 16U

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static bool valid_device(const bmi270_t *device)
{
    return device != NULL && device->initialized &&
        device->io.read != NULL && device->io.write != NULL;
}

static void delay_ms(unsigned ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t write_u8(bmi270_t *device, uint8_t reg, uint8_t value)
{
    return device->io.write(device->io.ctx, reg, &value, sizeof(value));
}

static esp_err_t read_u8(bmi270_t *device, uint8_t reg, uint8_t *value)
{
    return device->io.read(device->io.ctx, reg, value, sizeof(*value));
}

static esp_err_t upload_config(bmi270_t *device)
{
    esp_err_t ret = write_u8(device, REG_INIT_CTRL, 0x00U);
    if (ret != ESP_OK) {
        return ret;
    }
    size_t offset = 0U;
    while (offset < bmi270_config_file_size) {
        const uint16_t word_index = (uint16_t)(offset / 2U);
        const uint8_t addr[2] = {
            (uint8_t)(word_index & 0x0FU),
            (uint8_t)((word_index >> 4U) & 0xFFU),
        };
        ret = device->io.write(device->io.ctx, REG_INIT_ADDR_0, addr, sizeof(addr));
        if (ret != ESP_OK) {
            return ret;
        }
        size_t remain = bmi270_config_file_size - offset;
        if (remain > CHUNK) {
            remain = CHUNK;
        }
        ret = device->io.write(device->io.ctx, REG_INIT_DATA,
                               &bmi270_config_file[offset], remain);
        if (ret != ESP_OK) {
            return ret;
        }
        offset += remain;
    }
    ret = write_u8(device, REG_INIT_CTRL, 0x01U);
    if (ret != ESP_OK) {
        return ret;
    }
    delay_ms(150);
    uint8_t status = 0U;
    ret = read_u8(device, REG_INTERNAL_STATUS, &status);
    if (ret != ESP_OK) {
        return ret;
    }
    return ((status & 0x01U) != 0U) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t bmi270_init(bmi270_t *device, const bmi270_io_t *io)
{
    if (device == NULL || io == NULL || io->read == NULL || io->write == NULL ||
        bmi270_config_file_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    device->io = *io;
    uint8_t chip_id = 0U;
    esp_err_t ret = read_u8(device, REG_CHIP_ID, &chip_id);
    if (ret != ESP_OK) {
        return ret;
    }
    if (chip_id != BMI270_CHIP_ID) {
        return ESP_ERR_NOT_FOUND;
    }
    ret = write_u8(device, REG_CMD, CMD_SOFTRESET);
    if (ret != ESP_OK) {
        return ret;
    }
    delay_ms(6);
    /* Disable advanced power save so the config load can proceed. */
    ret = write_u8(device, REG_PWR_CONF, 0x00U);
    if (ret != ESP_OK) {
        return ret;
    }
    delay_ms(1);
    ret = upload_config(device);
    if (ret != ESP_OK) {
        return ret;
    }
    /* Accel 100 Hz, normal, ±4 g; gyro 100 Hz, normal, ±2000 dps. */
    ret = write_u8(device, REG_ACC_CONF, 0xA8U);
    if (ret == ESP_OK) {
        ret = write_u8(device, REG_ACC_RANGE, 0x01U);
    }
    if (ret == ESP_OK) {
        ret = write_u8(device, REG_GYR_CONF, 0xA9U);
    }
    if (ret == ESP_OK) {
        ret = write_u8(device, REG_GYR_RANGE, 0x00U);
    }
    if (ret == ESP_OK) {
        ret = write_u8(device, REG_PWR_CTRL, 0x0EU); /* temp + acc + gyr */
    }
    if (ret == ESP_OK) {
        ret = write_u8(device, REG_PWR_CONF, 0x02U); /* FIFO wakeup disabled, APS off */
    }
    if (ret != ESP_OK) {
        return ret;
    }
    device->initialized = true;
    return ESP_OK;
}

esp_err_t bmi270_deinit(bmi270_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    return ESP_OK;
}

esp_err_t bmi270_read_sample(bmi270_t *device, bmi270_sample_t *sample)
{
    if (!valid_device(device) || sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t raw[12] = {0};
    const esp_err_t ret = device->io.read(device->io.ctx, REG_DATA_8, raw, sizeof(raw));
    if (ret != ESP_OK) {
        return ret;
    }
    int16_t values[6];
    for (size_t i = 0; i < 6U; i++) {
        values[i] = (int16_t)((uint16_t)raw[i * 2U] | ((uint16_t)raw[i * 2U + 1U] << 8U));
    }
    /* ±4 g => 8192 LSB/g; ±2000 dps => 16.384 LSB/(dps) */
    const float accel_scale = 9.80665f / 8192.0f;
    const float gyro_scale = (float)(M_PI / 180.0 / 16.384);
    for (size_t i = 0; i < 3U; i++) {
        sample->accel_m_s2[i] = (float)values[i] * accel_scale;
        sample->gyro_rad_s[i] = (float)values[i + 3U] * gyro_scale;
    }
    sample->accel_valid = true;
    sample->gyro_valid = true;
    return ESP_OK;
}
