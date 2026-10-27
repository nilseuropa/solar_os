#include "lsm6ds3trc.h"
#include <string.h>

/* LSM6DS3TR-C: 104 Hz, +/-2 g, +/-250 degrees/s, little-endian samples. */
#define REG_WHO_AM_I 0x0FU
#define REG_CTRL1_XL 0x10U
#define REG_CTRL2_G 0x11U
#define REG_CTRL3_C 0x12U
#define REG_STATUS 0x1EU
#define REG_OUTX_L_G 0x22U
#define CTRL3_RESET 0x01U
#define CTRL3_BDU_INCREMENT 0x44U
#define ODR_104HZ 0x40U

static bool valid(const lsm6ds3trc_t *device) {
    return device && device->initialized && device->io.read && device->io.write;
}

static esp_err_t write_reg(lsm6ds3trc_t *device, uint8_t reg, uint8_t value) {
    return device->io.write(device->io.ctx, reg, &value, 1);
}

esp_err_t lsm6ds3trc_init(lsm6ds3trc_t *device, const lsm6ds3trc_io_t *io) {
    if (!device || !io || !io->read || !io->write || !io->delay_ms) return ESP_ERR_INVALID_ARG;
    memset(device, 0, sizeof(*device));
    device->io = *io;
    esp_err_t ret = io->read(io->ctx, REG_WHO_AM_I, &device->identity, 1);
    if (ret == ESP_OK && device->identity != LSM6DS3TRC_WHO_AM_I) ret = ESP_ERR_NOT_FOUND;
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    ret = write_reg(device, REG_CTRL3_C, CTRL3_RESET);
    if (ret == ESP_OK) {
        for (unsigned attempt = 0; attempt < 100; attempt++) {
            uint8_t control;
            ret = io->read(io->ctx, REG_CTRL3_C, &control, 1);
            if (ret != ESP_OK || !(control & CTRL3_RESET)) break;
            io->delay_ms(io->ctx, 1);
            if (attempt == 99) ret = ESP_ERR_TIMEOUT;
        }
    }
    if (ret == ESP_OK) ret = write_reg(device, REG_CTRL3_C, CTRL3_BDU_INCREMENT);
    if (ret == ESP_OK) ret = write_reg(device, REG_CTRL1_XL, ODR_104HZ);
    if (ret == ESP_OK) ret = write_reg(device, REG_CTRL2_G, ODR_104HZ);
    if (ret != ESP_OK) {
        (void)write_reg(device, REG_CTRL1_XL, 0);
        (void)write_reg(device, REG_CTRL2_G, 0);
        memset(device, 0, sizeof(*device));
        return ret;
    }
    device->initialized = true;
    return ESP_OK;
}

esp_err_t lsm6ds3trc_deinit(lsm6ds3trc_t *device) {
    if (!valid(device)) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = write_reg(device, REG_CTRL1_XL, 0);
    const esp_err_t gyro_ret = write_reg(device, REG_CTRL2_G, 0);
    if (ret == ESP_OK) ret = gyro_ret;
    if (ret == ESP_OK) memset(device, 0, sizeof(*device));
    return ret;
}

esp_err_t lsm6ds3trc_data_ready(lsm6ds3trc_t *device, bool *ready) {
    if (!valid(device) || !ready) return ESP_ERR_INVALID_ARG;
    uint8_t status;
    const esp_err_t ret = device->io.read(device->io.ctx, REG_STATUS, &status, 1);
    if (ret == ESP_OK) *ready = (status & 3U) == 3U;
    return ret;
}

static int16_t decode(const uint8_t *data) {
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

esp_err_t lsm6ds3trc_read_sample(lsm6ds3trc_t *device, lsm6ds3trc_sample_t *sample) {
    if (!valid(device) || !sample) return ESP_ERR_INVALID_ARG;
    uint8_t raw[12];
    const esp_err_t ret = device->io.read(device->io.ctx, REG_OUTX_L_G, raw, sizeof(raw));
    if (ret != ESP_OK) return ret;
    for (unsigned axis = 0; axis < 3; axis++) {
        sample->angular_velocity_rad_s[axis] = decode(raw + axis * 2) * (0.00875F * 0.01745329252F);
        sample->acceleration_m_s2[axis] = decode(raw + 6 + axis * 2) * (0.000061F * 9.80665F);
    }
    return ESP_OK;
}
