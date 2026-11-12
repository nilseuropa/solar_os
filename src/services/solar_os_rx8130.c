#include "solar_os_rx8130.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "rtc_rx8130.h"
#include "solar_os_buses.h"
#include "solar_os_rtc.h"

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    int irq_pin;
    esp_err_t last_read_error;
    rtc_rx8130_t rtc;
} solar_os_rx8130_device_t;

static solar_os_rx8130_device_t rtc_device;
static const char *TAG = "rx8130";

static esp_err_t chip_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    solar_os_rx8130_device_t *device = ctx;
    return solar_os_bus_i2c_read_reg(device->i2c_bus, device->address, reg, data, len);
}

static esp_err_t chip_write(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    solar_os_rx8130_device_t *device = ctx;
    return solar_os_bus_i2c_write_reg(device->i2c_bus, device->address, reg, data, len);
}

static esp_err_t rtc_get(void *user, solar_os_datetime_t *datetime)
{
    solar_os_rx8130_device_t *device = user;
    if (device == NULL || !device->active || datetime == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    rtc_rx8130_datetime_t value;
    const esp_err_t read_err = rtc_rx8130_get_datetime(&device->rtc, &value);
    if (read_err != ESP_OK) {
        if (read_err != device->last_read_error) {
            if (read_err == ESP_ERR_INVALID_RESPONSE) {
                ESP_LOGW(TAG, "RTC calendar invalid or unset; use date YYYY-MM-DD "
                         "then time HH:MM:SS, or ntp");
            } else {
                ESP_LOGE(TAG, "rtc_get: read failed: %s", esp_err_to_name(read_err));
            }
        }
        device->last_read_error = read_err;
        return read_err;
    }
    device->last_read_error = ESP_OK;
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
    solar_os_rx8130_device_t *device = user;
    if (device == NULL || !device->active || datetime == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const rtc_rx8130_datetime_t value = {
        .year = datetime->year,
        .month = datetime->month,
        .day = datetime->day,
        .hour = datetime->hour,
        .minute = datetime->minute,
        .second = datetime->second,
        .weekday = datetime->weekday,
        .clock_integrity = datetime->clock_integrity,
    };
    return rtc_rx8130_set_datetime(&device->rtc, &value);
}

static esp_err_t rtc_set_alarm(void *user, const solar_os_rtc_alarm_t *alarm)
{
    solar_os_rx8130_device_t *device = user;
    if (device == NULL || !device->active || alarm == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t match_fields = 0;
    if ((alarm->match_fields & SOLAR_OS_RTC_ALARM_MATCH_MINUTE) != 0) {
        match_fields |= RTC_RX8130_ALARM_MATCH_MINUTE;
    }
    if ((alarm->match_fields & SOLAR_OS_RTC_ALARM_MATCH_HOUR) != 0) {
        match_fields |= RTC_RX8130_ALARM_MATCH_HOUR;
    }
    if ((alarm->match_fields & SOLAR_OS_RTC_ALARM_MATCH_DAY) != 0) {
        match_fields |= RTC_RX8130_ALARM_MATCH_DAY;
    }
    if ((alarm->match_fields & SOLAR_OS_RTC_ALARM_MATCH_WEEKDAY) != 0) {
        match_fields |= RTC_RX8130_ALARM_MATCH_WEEKDAY;
    }
    const rtc_rx8130_alarm_t value = {
        .match_fields = match_fields,
        .second = alarm->second,
        .minute = alarm->minute,
        .hour = alarm->hour,
        .day = alarm->day,
        .weekday = alarm->weekday,
    };
    return rtc_rx8130_set_alarm(&device->rtc, &value);
}

static esp_err_t rtc_disable_alarm(void *user)
{
    solar_os_rx8130_device_t *device = user;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    return rtc_rx8130_disable_alarm(&device->rtc);
}

static esp_err_t rtc_set_countdown(void *user, uint32_t period_seconds, bool repeat)
{
    solar_os_rx8130_device_t *device = user;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    return rtc_rx8130_set_countdown(&device->rtc, period_seconds, repeat);
}

static esp_err_t rtc_disable_countdown(void *user)
{
    solar_os_rx8130_device_t *device = user;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    return rtc_rx8130_disable_countdown(&device->rtc);
}

esp_err_t solar_os_rx8130_attach(const char *name,
                                 const solar_os_expansion_binding_t *bindings,
                                 size_t binding_count)
{
    if (name == NULL || name[0] == '\0' || rtc_device.active) {
        return ESP_ERR_INVALID_ARG;
    }
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;
    int irq_pin = -1;
    bool have_i2c = false;
    bool have_address = false;
    for (size_t i = 0; bindings != NULL && i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_i2c) {
            strlcpy(i2c_bus, binding->target, sizeof(i2c_bus));
            have_i2c = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address) {
            address = (uint8_t)binding->value;
            have_address = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   strcmp(binding->role, "irq") == 0) {
            irq_pin = binding->value;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (!have_i2c || !have_address ||
        !solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_probe(i2c_bus, address),
                        TAG, "RX8130 not found");
    memset(&rtc_device, 0, sizeof(rtc_device));
    rtc_device.address = address;
    rtc_device.irq_pin = irq_pin;
    strlcpy(rtc_device.name, name, sizeof(rtc_device.name));
    strlcpy(rtc_device.i2c_bus, i2c_bus, sizeof(rtc_device.i2c_bus));
    const rtc_rx8130_io_t io = {
        .read = chip_read,
        .write = chip_write,
        .ctx = &rtc_device,
    };
    ESP_RETURN_ON_ERROR(rtc_rx8130_init(&rtc_device.rtc, &io),
                        TAG, "controller init failed");
    rtc_device.active = true;
    const solar_os_rtc_provider_t provider = {
        .get_utc_datetime = rtc_get,
        .set_utc_datetime = rtc_set,
        .set_alarm = rtc_set_alarm,
        .disable_alarm = rtc_disable_alarm,
        .set_countdown = rtc_set_countdown,
        .disable_countdown = rtc_disable_countdown,
        .user = &rtc_device,
        .interrupt_gpio = rtc_device.irq_pin,
        .interrupt_active_level = 0,
    };
    const esp_err_t ret = solar_os_rtc_register_provider(name, &provider);
    if (ret != ESP_OK) {
        (void)rtc_rx8130_deinit(&rtc_device.rtc);
        memset(&rtc_device, 0, sizeof(rtc_device));
    }
    return ret;
}

esp_err_t solar_os_rx8130_detach(const char *name)
{
    if (!rtc_device.active || name == NULL ||
        strcmp(rtc_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(solar_os_rtc_unregister_provider(name),
                        TAG, "provider unregister failed");
    (void)rtc_rx8130_deinit(&rtc_device.rtc);
    memset(&rtc_device, 0, sizeof(rtc_device));
    return ESP_OK;
}
