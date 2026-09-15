#include "drv2605.h"

#include <string.h>

#define DRV2605_REG_STATUS 0x00U
#define DRV2605_REG_MODE 0x01U
#define DRV2605_REG_LIBRARY 0x03U
#define DRV2605_REG_WAVESEQ1 0x04U
#define DRV2605_REG_WAVESEQ2 0x05U
#define DRV2605_REG_GO 0x0CU
#define DRV2605_REG_FEEDBACK 0x1AU

#define DRV2605_MODE_INTERNAL_TRIGGER 0x00U
#define DRV2605_LIBRARY_ERM 0x01U
#define DRV2605_FEEDBACK_LRA 0x80U

#define DRV2605_CHIP_ID 0x03U
#define DRV2605X_CHIP_ID 0x05U
#define DRV2605L_CHIP_ID 0x07U

static bool io_valid(const drv2605_io_t *io)
{
    return io != NULL && io->read != NULL && io->write != NULL;
}

static bool device_valid(const drv2605_t *device)
{
    return device != NULL && device->initialized && io_valid(&device->io);
}

static esp_err_t read_u8(drv2605_t *device, uint8_t reg, uint8_t *value)
{
    return device->io.read(device->io.ctx, reg, value, 1U);
}

static esp_err_t write_u8(drv2605_t *device, uint8_t reg, uint8_t value)
{
    return device->io.write(device->io.ctx, reg, &value, 1U);
}

static bool supported_chip_id(uint8_t chip_id)
{
    return chip_id == DRV2605_CHIP_ID || chip_id == DRV2605X_CHIP_ID ||
        chip_id == DRV2605L_CHIP_ID;
}

esp_err_t drv2605_init(drv2605_t *device, const drv2605_io_t *io)
{
    if (device == NULL || !io_valid(io)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    device->io = *io;

    uint8_t status = 0U;
    esp_err_t ret = read_u8(device, DRV2605_REG_STATUS, &status);
    if (ret != ESP_OK) {
        goto fail;
    }
    device->chip_id = status >> 5U;
    if (!supported_chip_id(device->chip_id)) {
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    ret = write_u8(device, DRV2605_REG_MODE,
                   DRV2605_MODE_INTERNAL_TRIGGER);
    if (ret != ESP_OK) {
        goto fail;
    }
    if (device->io.delay_ms != NULL) {
        device->io.delay_ms(device->io.ctx, 1U);
    }
    ret = write_u8(device, DRV2605_REG_LIBRARY, DRV2605_LIBRARY_ERM);
    if (ret != ESP_OK) {
        goto fail;
    }
    uint8_t feedback = 0U;
    ret = read_u8(device, DRV2605_REG_FEEDBACK, &feedback);
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = write_u8(device,
                   DRV2605_REG_FEEDBACK,
                   feedback & (uint8_t)~DRV2605_FEEDBACK_LRA);
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = write_u8(device, DRV2605_REG_WAVESEQ1, 0U);
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = write_u8(device, DRV2605_REG_WAVESEQ2, 0U);
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = write_u8(device, DRV2605_REG_GO, 0U);
    if (ret != ESP_OK) {
        goto fail;
    }
    device->initialized = true;
    return ESP_OK;

fail:
    memset(device, 0, sizeof(*device));
    return ret;
}

esp_err_t drv2605_deinit(drv2605_t *device)
{
    if (!device_valid(device)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t ret = write_u8(device, DRV2605_REG_GO, 0U);
    if (ret == ESP_OK) {
        memset(device, 0, sizeof(*device));
    }
    return ret;
}

esp_err_t drv2605_play_effect(drv2605_t *device, uint16_t effect)
{
    if (!device_valid(device)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (effect == 0U || effect > DRV2605_EFFECT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = write_u8(device, DRV2605_REG_GO, 0U);
    if (ret == ESP_OK) {
        ret = write_u8(device, DRV2605_REG_WAVESEQ1, (uint8_t)effect);
    }
    if (ret == ESP_OK) {
        ret = write_u8(device, DRV2605_REG_WAVESEQ2, 0U);
    }
    if (ret == ESP_OK) {
        ret = write_u8(device, DRV2605_REG_GO, 1U);
    }
    return ret;
}

esp_err_t drv2605_stop(drv2605_t *device)
{
    return device_valid(device) ?
        write_u8(device, DRV2605_REG_GO, 0U) : ESP_ERR_INVALID_STATE;
}
