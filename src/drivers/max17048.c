#include "max17048.h"

#include <string.h>

#define MAX17048_REG_VCELL 0x02U
#define MAX17048_REG_SOC 0x04U
#define MAX17048_REG_VERSION 0x08U

static uint16_t read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
}

esp_err_t max17048_init(max17048_t *device, const max17048_io_t *io)
{
    if (device == NULL || io == NULL || io->read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(device, 0, sizeof(*device));
    device->io = *io;

    uint8_t version[2];
    const esp_err_t ret = device->io.read(device->io.user,
                                          MAX17048_REG_VERSION,
                                          version,
                                          sizeof(version));
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    device->version = read_be16(version);
    device->initialized = true;
    return ESP_OK;
}

esp_err_t max17048_read_sample(max17048_t *device,
                               max17048_sample_t *sample)
{
    if (device == NULL || !device->initialized || sample == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t vcell_data[2];
    esp_err_t ret = device->io.read(device->io.user,
                                    MAX17048_REG_VCELL,
                                    vcell_data,
                                    sizeof(vcell_data));
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t soc_data[2];
    ret = device->io.read(device->io.user,
                          MAX17048_REG_SOC,
                          soc_data,
                          sizeof(soc_data));
    if (ret != ESP_OK) {
        return ret;
    }

    const uint16_t vcell_raw = read_be16(vcell_data);
    const uint16_t soc_raw = read_be16(soc_data);
    uint16_t percent = (uint16_t)((soc_raw + 128U) >> 8U);
    if (percent > 100U) {
        percent = 100U;
    }
    *sample = (max17048_sample_t) {
        /* VCELL is 78.125 uV per raw LSB, or 5/64 mV. */
        .voltage_mv = (uint16_t)(((uint32_t)vcell_raw * 5U + 32U) / 64U),
        .percent = (uint8_t)percent,
        .soc_raw = soc_raw,
    };
    return ESP_OK;
}
