#include "solar_os_pcf85063.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "rtc_pcf85063.h"
#include "solar_os_time.h"

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    rtc_pcf85063_t rtc;
} solar_os_pcf85063_device_t;

static solar_os_pcf85063_device_t rtc_device;

static esp_err_t rtc_get(void *user, solar_os_datetime_t *datetime)
{
    solar_os_pcf85063_device_t *device = user;
    if (device == NULL || !device->active || datetime == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    rtc_datetime_t value;
    ESP_RETURN_ON_ERROR(rtc_pcf85063_get_datetime_device(&device->rtc, &value),
                        "pcf85063",
                        "read failed");
    *datetime = (solar_os_datetime_t) {
        .year = value.year,
        .month = value.month,
        .day = value.day,
        .hour = value.hour,
        .minute = value.minute,
        .second = value.second,
        .weekday = value.weekday,
        .clock_integrity = value.clock_integrity,
    };
    return ESP_OK;
}

static esp_err_t rtc_set(void *user, const solar_os_datetime_t *datetime)
{
    solar_os_pcf85063_device_t *device = user;
    if (device == NULL || !device->active || datetime == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const rtc_datetime_t value = {
        .year = datetime->year,
        .month = datetime->month,
        .day = datetime->day,
        .hour = datetime->hour,
        .minute = datetime->minute,
        .second = datetime->second,
        .weekday = datetime->weekday,
        .clock_integrity = datetime->clock_integrity,
    };
    return rtc_pcf85063_set_datetime_device(&device->rtc, &value);
}

esp_err_t solar_os_pcf85063_attach(const char *name,
                                    const solar_os_expansion_binding_t *bindings,
                                    size_t binding_count)
{
    if (name == NULL || name[0] == '\0' || bindings == NULL ||
        rtc_device.active) {
        return rtc_device.active ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
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
    if (bus == NULL || address != RTC_PCF85063_ADDRESS) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(rtc_pcf85063_init_device(&rtc_device.rtc,
                                                  bus,
                                                  (uint8_t)address),
                        "pcf85063",
                        "controller init failed");
    strlcpy(rtc_device.name, name, sizeof(rtc_device.name));
    rtc_device.active = true;
    const solar_os_time_provider_t provider = {
        .get_utc_datetime = rtc_get,
        .set_utc_datetime = rtc_set,
        .user = &rtc_device,
    };
    const esp_err_t ret = solar_os_time_register_provider(name, &provider);
    if (ret != ESP_OK) {
        memset(&rtc_device, 0, sizeof(rtc_device));
    }
    return ret;
}

esp_err_t solar_os_pcf85063_detach(const char *name)
{
    if (!rtc_device.active || name == NULL ||
        strcmp(rtc_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(solar_os_time_unregister_provider(name),
                        "pcf85063",
                        "provider unregister failed");
    memset(&rtc_device, 0, sizeof(rtc_device));
    return ESP_OK;
}
