#include "solar_os_battery_adc.h"

#include <stdbool.h>
#include <string.h>

#include "adc_port.h"
#include "esp_check.h"
#include "solar_os_battery.h"

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    int pin;
    int divider_milli;
} solar_os_battery_adc_device_t;

static solar_os_battery_adc_device_t battery_device;

static esp_err_t battery_read(void *user, solar_os_battery_sample_t *sample)
{
    solar_os_battery_adc_device_t *device = user;
    if (device == NULL || !device->active || sample == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    adc_port_sample_t adc_sample;
    ESP_RETURN_ON_ERROR(adc_port_read((gpio_num_t)device->pin, &adc_sample),
                        "battery-adc",
                        "ADC read failed");
    if (!adc_sample.calibrated) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint32_t battery_mv =
        ((uint32_t)adc_sample.voltage_mv * (uint32_t)device->divider_milli + 500U) /
        1000U;
    sample->battery_mv = (uint16_t)(battery_mv > UINT16_MAX ? UINT16_MAX : battery_mv);
    sample->calibrated = true;
    return ESP_OK;
}

esp_err_t solar_os_battery_adc_attach(const char *name,
                                       const solar_os_expansion_binding_t *bindings,
                                       size_t binding_count)
{
    if (name == NULL || name[0] == '\0' || bindings == NULL ||
        battery_device.active) {
        return battery_device.active ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
    }
    int pin = -1;
    int divider_milli = -1;
    for (size_t i = 0; i < binding_count; i++) {
        if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_ADC && pin < 0) {
            pin = bindings[i].value;
        } else if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(bindings[i].role, "divider") == 0 && divider_milli < 0) {
            divider_milli = bindings[i].value;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (pin < 0 || divider_milli < 1000 || divider_milli > 10000) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(adc_port_configure_pin((gpio_num_t)pin,
                                                ADC_ATTEN_DB_12,
                                                ADC_BITWIDTH_12),
                        "battery-adc",
                        "ADC configuration failed");

    battery_device.pin = pin;
    battery_device.divider_milli = divider_milli;
    strlcpy(battery_device.name, name, sizeof(battery_device.name));
    battery_device.active = true;
    const solar_os_battery_provider_t provider = {
        .read = battery_read,
        .user = &battery_device,
    };
    const esp_err_t ret = solar_os_battery_register_provider(name, &provider);
    if (ret != ESP_OK) {
        memset(&battery_device, 0, sizeof(battery_device));
    }
    return ret;
}

esp_err_t solar_os_battery_adc_detach(const char *name)
{
    if (!battery_device.active || name == NULL ||
        strcmp(battery_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(solar_os_battery_unregister_provider(name),
                        "battery-adc",
                        "provider unregister failed");
    memset(&battery_device, 0, sizeof(battery_device));
    return ESP_OK;
}
