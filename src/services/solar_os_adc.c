#include "solar_os_adc.h"

#include <stdio.h>

#include "solar_os_board_caps.h"
#include "solar_os_config.h"
#include "solar_os_resources.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if SOLAR_OS_BOARD_HAS_ADC
#include "adc_port.h"
#include "driver/gpio.h"
#include "hal/adc_types.h"
#endif

#include "solar_os_gpio.h"

esp_err_t solar_os_adc_init(void)
{
#if !SOLAR_OS_BOARD_HAS_ADC
    return ESP_ERR_NOT_SUPPORTED;
#else
    return adc_port_init();
#endif
}

size_t solar_os_adc_pin_count(void)
{
#if !SOLAR_OS_BOARD_HAS_ADC
    return 0;
#else
    size_t count = 0;
    for (size_t i = 0; i < solar_os_gpio_pin_count(); i++) {
        solar_os_gpio_pin_info_t gpio_info;
        if (solar_os_gpio_get_pin_info(i, &gpio_info) && gpio_info.runtime_allowed) {
            count++;
        }
    }
    return count;
#endif
}

bool solar_os_adc_get_pin_info(size_t index, solar_os_adc_pin_info_t *info)
{
#if !SOLAR_OS_BOARD_HAS_ADC
    (void)index;
    (void)info;
    return false;
#else
    if (info == NULL) {
        return false;
    }

    size_t current = 0;
    for (size_t i = 0; i < solar_os_gpio_pin_count(); i++) {
        solar_os_gpio_pin_info_t gpio_info;
        if (!solar_os_gpio_get_pin_info(i, &gpio_info) || !gpio_info.runtime_allowed) {
            continue;
        }
        if (current++ != index) {
            continue;
        }

        adc_unit_t unit = 0;
        adc_channel_t channel = 0;
        const bool capable = adc_port_is_adc_capable((gpio_num_t)gpio_info.pin, &unit, &channel);
        *info = (solar_os_adc_pin_info_t) {
            .pin = gpio_info.pin,
            .runtime_allowed = true,
            .adc_capable = capable,
            .unit = capable ? (int)unit + 1 : -1,
            .channel = capable ? (int)channel : -1,
        };
        return true;
    }
    return false;
#endif
}

esp_err_t solar_os_adc_read(int pin, solar_os_adc_sample_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_BOARD_HAS_ADC
    (void)pin;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!solar_os_gpio_is_runtime_allowed(pin)) {
        return ESP_ERR_NOT_ALLOWED;
    }

    char owner[SOLAR_OS_RESOURCE_OWNER_MAX];
    snprintf(owner, sizeof(owner), "adc:%d:%p", pin, xTaskGetCurrentTaskHandle());
    esp_err_t ret = ESP_OK;
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    const solar_os_resource_request_t requests[] = {
        {
            .kind = SOLAR_OS_RESOURCE_GPIO_PIN,
            .primary = pin,
            .secondary = -1,
            .label = "adc-gpio",
        },
        {
            .kind = SOLAR_OS_RESOURCE_ADC_PIN,
            .primary = pin,
            .secondary = -1,
            .label = "adc",
        },
    };
    ret = solar_os_resource_claim_bundle(requests,
                                         sizeof(requests) / sizeof(requests[0]),
                                         owner,
                                         NULL);
    if (ret != ESP_OK) {
        return ret;
    }
#endif

    adc_port_sample_t port_sample;
    ret = adc_port_read((gpio_num_t)pin, &port_sample);
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    (void)solar_os_resource_release_owner(owner);
#endif
    if (ret != ESP_OK) {
        return ret;
    }

    *sample = (solar_os_adc_sample_t) {
        .pin = port_sample.pin,
        .raw = port_sample.raw,
        .voltage_mv = port_sample.voltage_mv,
        .unit = (int)port_sample.unit + 1,
        .channel = (int)port_sample.channel,
        .calibrated = port_sample.calibrated,
    };
    return ESP_OK;
#endif
}
