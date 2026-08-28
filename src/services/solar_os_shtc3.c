#include "solar_os_shtc3.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "shtc3.h"
#include "solar_os_sensors.h"

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    shtc3_t sensor;
} solar_os_shtc3_device_t;

static solar_os_shtc3_device_t sensor_device;

static esp_err_t sensor_read(void *user, solar_os_environment_t *environment)
{
    solar_os_shtc3_device_t *device = user;
    if (device == NULL || !device->active || environment == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    shtc3_measurement_t measurement;
    ESP_RETURN_ON_ERROR(shtc3_read_measurement_device(&device->sensor, &measurement),
                        "shtc3",
                        "measurement failed");
    environment->temperature_c = measurement.temperature_c;
    environment->humidity_percent = measurement.humidity_percent;
    return ESP_OK;
}

esp_err_t solar_os_shtc3_attach(const char *name,
                                 const solar_os_expansion_binding_t *bindings,
                                 size_t binding_count)
{
    if (name == NULL || name[0] == '\0' || bindings == NULL ||
        sensor_device.active) {
        return sensor_device.active ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
    }
    const char *bus = NULL;
    int address = -1;
    for (size_t i = 0; i < binding_count; i++) {
        if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && bus == NULL) {
            bus = bindings[i].target;
        } else if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   address < 0) {
            address = bindings[i].value;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (bus == NULL || address != SHTC3_ADDRESS) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(shtc3_init_device(&sensor_device.sensor,
                                           bus,
                                           (uint8_t)address),
                        "shtc3",
                        "controller init failed");
    strlcpy(sensor_device.name, name, sizeof(sensor_device.name));
    sensor_device.active = true;
    const solar_os_sensors_provider_t provider = {
        .read_environment = sensor_read,
        .user = &sensor_device,
        .has_temperature = true,
        .has_humidity = true,
    };
    const esp_err_t ret = solar_os_sensors_register_provider(name, &provider);
    if (ret != ESP_OK) {
        memset(&sensor_device, 0, sizeof(sensor_device));
    }
    return ret;
}

esp_err_t solar_os_shtc3_detach(const char *name)
{
    if (!sensor_device.active || name == NULL ||
        strcmp(sensor_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(solar_os_sensors_unregister_provider(name),
                        "shtc3",
                        "provider unregister failed");
    memset(&sensor_device, 0, sizeof(sensor_device));
    return ESP_OK;
}
