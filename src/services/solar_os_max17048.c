#include "solar_os_max17048.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "max17048.h"
#include "solar_os_battery.h"
#include "solar_os_buses.h"

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    max17048_t gauge;
} solar_os_max17048_device_t;

static const char *TAG = "max17048";
static solar_os_max17048_device_t gauge_device;

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *address)
{
    bool have_i2c = false;
    bool have_address = false;
    if (bindings == NULL || i2c_bus == NULL || address == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_bus[0] = '\0';
    *address = 0U;
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_i2c) {
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address && binding->value == MAX17048_I2C_ADDRESS) {
            *address = (uint8_t)binding->value;
            have_address = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_i2c && have_address &&
            solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

static esp_err_t gauge_read_reg(void *user,
                                uint8_t reg,
                                uint8_t *data,
                                size_t len)
{
    solar_os_max17048_device_t *device = user;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    return solar_os_bus_i2c_read_reg(device->i2c_bus,
                                     device->address,
                                     reg,
                                     data,
                                     len);
}

static esp_err_t battery_read(void *user, solar_os_battery_sample_t *sample)
{
    solar_os_max17048_device_t *device = user;
    if (device == NULL || !device->active || sample == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    max17048_sample_t value;
    ESP_RETURN_ON_ERROR(max17048_read_sample(&device->gauge, &value),
                        TAG,
                        "sample read failed");
    *sample = (solar_os_battery_sample_t) {
        .battery_mv = value.voltage_mv,
        .calibrated = true,
        .percent_valid = true,
        .percent = value.percent,
    };
    return ESP_OK;
}

static void clear_device(void)
{
    memset(&gauge_device, 0, sizeof(gauge_device));
}

esp_err_t solar_os_max17048_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (gauge_device.active) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       i2c_bus,
                                       sizeof(i2c_bus),
                                       &address),
                        TAG,
                        "invalid bindings");
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_probe(i2c_bus, address),
                        TAG,
                        "MAX17048 not found");

    clear_device();
    gauge_device.active = true;
    gauge_device.address = address;
    strlcpy(gauge_device.name, name, sizeof(gauge_device.name));
    strlcpy(gauge_device.i2c_bus, i2c_bus, sizeof(gauge_device.i2c_bus));
    const max17048_io_t io = {
        .read = gauge_read_reg,
        .user = &gauge_device,
    };
    esp_err_t ret = max17048_init(&gauge_device.gauge, &io);
    if (ret != ESP_OK) {
        clear_device();
        return ret;
    }

    const solar_os_battery_provider_t provider = {
        .read = battery_read,
        .user = &gauge_device,
    };
    ret = solar_os_battery_register_provider(name, &provider);
    if (ret != ESP_OK) {
        clear_device();
        return ret;
    }
    ESP_LOGI(TAG,
             "%s attached on %s address 0x%02x version 0x%04x",
             name,
             i2c_bus,
             address,
             gauge_device.gauge.version);
    return ESP_OK;
}

esp_err_t solar_os_max17048_detach(const char *name)
{
    if (!gauge_device.active || name == NULL ||
        strcmp(gauge_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(solar_os_battery_unregister_provider(name),
                        TAG,
                        "unregister failed");
    clear_device();
    return ESP_OK;
}
