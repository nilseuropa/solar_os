#include "sht40.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

#include "../../modules/examples/sht40-driver/main/sht40_protocol.h"

static sht40_t default_device;

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

    vTaskDelay(pdMS_TO_TICKS(SHT40_MEASURE_DELAY_MS));

    uint8_t data[6] = {0};
    ret = solar_os_bus_i2c_receive(device->bus,
                                   device->address,
                                   data,
                                   sizeof(data));
    if (ret != ESP_OK) {
        return ret;
    }

    if (!sht40_decode_sample(data, &measurement->temperature_c, &measurement->humidity_percent)) {
        return ESP_ERR_INVALID_RESPONSE;
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
