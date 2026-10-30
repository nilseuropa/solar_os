#include "sht40.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

#define SHT40_CMD_MEASURE_HIGH 0xfd
#define SHT40_CMD_SOFT_RESET 0x94
#define SHT40_TEMPERATURE_OFFSET_C 0.0f

static sht40_t default_device;

static uint8_t sht40_crc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xff;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if ((crc & 0x80) != 0) {
                crc = (uint8_t)((crc << 1) ^ 0x31);
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

static bool sht40_crc_ok(const uint8_t *data, uint8_t checksum)
{
    return sht40_crc(data, 2) == checksum;
}

static esp_err_t sht40_write_cmd(const sht40_t *device, uint8_t command)
{
    if (device == NULL || device->bus[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    return solar_os_bus_i2c_transmit(device->bus,
                                     device->address,
                                     &command,
                                     1);
}

esp_err_t sht40_read_measurement_device(const sht40_t *device,
                                        sht40_measurement_t *measurement)
{
    if (device == NULL || device->bus[0] == '\0' || measurement == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = sht40_write_cmd(device, SHT40_CMD_MEASURE_HIGH);
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t data[6] = {0};
    ret = solar_os_bus_i2c_receive(device->bus,
                                   device->address,
                                   data,
                                   sizeof(data));
    if (ret != ESP_OK) {
        return ret;
    }

    if (!sht40_crc_ok(data, data[2]) || !sht40_crc_ok(data + 3, data[5])) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint16_t raw_temp = ((uint16_t)data[0] << 8) | data[1];
    const uint16_t raw_hum = ((uint16_t)data[3] << 8) | data[4];

    measurement->temperature_c = -45.0f + 175.0f * ((float)raw_temp / 65535.0f) + SHT40_TEMPERATURE_OFFSET_C;
    measurement->humidity_percent = -6.0f + 125.0f * ((float)raw_hum / 65535.0f);
    if (measurement->humidity_percent < 0.0f) {
        measurement->humidity_percent = 0.0f;
    } else if (measurement->humidity_percent > 100.0f) {
        measurement->humidity_percent = 100.0f;
    }
    measurement->id = 0;

    return ESP_OK;
}

esp_err_t sht40_init_device(sht40_t *device, const char *bus, uint8_t address)
{
    if (device == NULL || bus == NULL || bus[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(device, 0, sizeof(*device));
    strncpy(device->bus, bus, sizeof(device->bus) - 1);
    device->address = address;

    return sht40_write_cmd(device, SHT40_CMD_SOFT_RESET);
}

esp_err_t sht40_init(void)
{
    return sht40_init_device(&default_device, "i2c0", SHT40_ADDRESS);
}

esp_err_t sht40_read_measurement(sht40_measurement_t *measurement)
{
    return sht40_read_measurement_device(&default_device, measurement);
}

esp_err_t sht40_read_id_device(const sht40_t *device, uint16_t *id)
{
    if (id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *id = 0;
    return ESP_OK;
}

esp_err_t sht40_read_id(uint16_t *id)
{
    return sht40_read_id_device(&default_device, id);
}
