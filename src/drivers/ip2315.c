#include "ip2315.h"

#include <string.h>

static bool valid_device(const ip2315_t *device)
{
    return device != NULL && device->initialized &&
        device->io.read != NULL && device->io.write != NULL;
}

esp_err_t ip2315_init(ip2315_t *device, const ip2315_io_t *io)
{
    if (device == NULL || io == NULL || io->read == NULL || io->write == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    device->io = *io;
    device->initialized = true;
    return ESP_OK;
}

esp_err_t ip2315_deinit(ip2315_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    return ESP_OK;
}

esp_err_t ip2315_set_charge_enabled(ip2315_t *device, bool enabled)
{
    if (!valid_device(device)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value = 0U;
    esp_err_t ret = device->io.read(device->io.ctx, IP2315_REG_CHARGE_EN, &value, 1U);
    if (ret != ESP_OK) {
        return ret;
    }
    if (enabled) {
        value = (uint8_t)(value | 0x01U);
    } else {
        value = (uint8_t)(value & (uint8_t)~0x01U);
    }
    return device->io.write(device->io.ctx, IP2315_REG_CHARGE_EN, &value, 1U);
}

esp_err_t ip2315_read_charge_stat(ip2315_t *device, uint8_t *stat)
{
    if (!valid_device(device) || stat == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return device->io.read(device->io.ctx, IP2315_REG_CHG_STAT, stat, 1U);
}
