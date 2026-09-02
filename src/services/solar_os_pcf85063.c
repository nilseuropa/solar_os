#include "solar_os_pcf85063.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "rtc_pcf85063.h"
#include "solar_os_rtc.h"

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    rtc_pcf85063_t rtc;
    int irq_pin;
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

static esp_err_t rtc_set_alarm(void *user, const solar_os_rtc_alarm_t *alarm)
{
    solar_os_pcf85063_device_t *device = user;
    if (device == NULL || !device->active || alarm == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t match_fields = 0;
    if ((alarm->match_fields & SOLAR_OS_RTC_ALARM_MATCH_SECOND) != 0) {
        match_fields |= RTC_PCF85063_ALARM_MATCH_SECOND;
    }
    if ((alarm->match_fields & SOLAR_OS_RTC_ALARM_MATCH_MINUTE) != 0) {
        match_fields |= RTC_PCF85063_ALARM_MATCH_MINUTE;
    }
    if ((alarm->match_fields & SOLAR_OS_RTC_ALARM_MATCH_HOUR) != 0) {
        match_fields |= RTC_PCF85063_ALARM_MATCH_HOUR;
    }
    if ((alarm->match_fields & SOLAR_OS_RTC_ALARM_MATCH_DAY) != 0) {
        match_fields |= RTC_PCF85063_ALARM_MATCH_DAY;
    }
    if ((alarm->match_fields & SOLAR_OS_RTC_ALARM_MATCH_WEEKDAY) != 0) {
        match_fields |= RTC_PCF85063_ALARM_MATCH_WEEKDAY;
    }
    const rtc_pcf85063_alarm_t value = {
        .match_fields = match_fields,
        .second = alarm->second,
        .minute = alarm->minute,
        .hour = alarm->hour,
        .day = alarm->day,
        .weekday = alarm->weekday,
    };
    return rtc_pcf85063_set_alarm_device(&device->rtc, &value);
}

static esp_err_t rtc_disable_alarm(void *user)
{
    solar_os_pcf85063_device_t *device = user;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    return rtc_pcf85063_disable_alarm_device(&device->rtc);
}

static esp_err_t rtc_set_countdown(void *user, uint32_t period_seconds, bool repeat)
{
    solar_os_pcf85063_device_t *device = user;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    return rtc_pcf85063_set_countdown_device(&device->rtc,
                                             period_seconds,
                                             repeat);
}

static esp_err_t rtc_disable_countdown(void *user)
{
    solar_os_pcf85063_device_t *device = user;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    return rtc_pcf85063_disable_countdown_device(&device->rtc);
}

static esp_err_t rtc_get_interrupt_status(void *user, uint32_t *interrupts)
{
    solar_os_pcf85063_device_t *device = user;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t device_interrupts = 0;
    const esp_err_t ret = rtc_pcf85063_get_interrupt_status_device(
        &device->rtc,
        &device_interrupts);
    if (ret != ESP_OK) {
        return ret;
    }
    *interrupts = 0;
    if ((device_interrupts & RTC_PCF85063_INTERRUPT_ALARM) != 0) {
        *interrupts |= SOLAR_OS_RTC_INTERRUPT_ALARM;
    }
    if ((device_interrupts & RTC_PCF85063_INTERRUPT_COUNTDOWN) != 0) {
        *interrupts |= SOLAR_OS_RTC_INTERRUPT_COUNTDOWN;
    }
    return ESP_OK;
}

static esp_err_t rtc_clear_interrupt_status(void *user, uint32_t interrupts)
{
    solar_os_pcf85063_device_t *device = user;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t device_interrupts = 0;
    if ((interrupts & SOLAR_OS_RTC_INTERRUPT_ALARM) != 0) {
        device_interrupts |= RTC_PCF85063_INTERRUPT_ALARM;
    }
    if ((interrupts & SOLAR_OS_RTC_INTERRUPT_COUNTDOWN) != 0) {
        device_interrupts |= RTC_PCF85063_INTERRUPT_COUNTDOWN;
    }
    return rtc_pcf85063_clear_interrupt_status_device(&device->rtc,
                                                       device_interrupts);
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
    int irq_pin = -1;
    for (size_t i = 0; i < binding_count; i++) {
        if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && bus == NULL) {
            bus = bindings[i].target;
        } else if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   address < 0) {
            address = bindings[i].value;
        } else if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   strcmp(bindings[i].role, "irq") == 0 && irq_pin < 0) {
            irq_pin = bindings[i].value;
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
    rtc_device.irq_pin = irq_pin;
    rtc_device.active = true;
    const solar_os_rtc_provider_t provider = {
        .get_utc_datetime = rtc_get,
        .set_utc_datetime = rtc_set,
        .set_alarm = rtc_set_alarm,
        .disable_alarm = rtc_disable_alarm,
        .set_countdown = rtc_set_countdown,
        .disable_countdown = rtc_disable_countdown,
        .get_interrupt_status = rtc_get_interrupt_status,
        .clear_interrupt_status = rtc_clear_interrupt_status,
        .user = &rtc_device,
        .interrupt_gpio = rtc_device.irq_pin,
    };
    const esp_err_t ret = solar_os_rtc_register_provider(name, &provider);
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
    ESP_RETURN_ON_ERROR(solar_os_rtc_unregister_provider(name),
                        "pcf85063",
                        "provider unregister failed");
    memset(&rtc_device, 0, sizeof(rtc_device));
    return ESP_OK;
}
