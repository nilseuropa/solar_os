#include "rtc_pcf85063.h"

#include <string.h>

#include "solar_os_buses.h"

#define PCF85063_CTRL1_REG 0x00
#define PCF85063_RAM_REG 0x03
#define PCF85063_SEC_REG 0x04
#define PCF85063_CTRL1_STOP_BIT 0x20
#define PCF85063_CTRL1_12H_BIT 0x02
#define PCF85063_SECONDS_OS_BIT 0x80

static rtc_pcf85063_t default_device;

static uint8_t bcd_to_dec(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0f));
}

static uint8_t dec_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static bool is_leap_year(uint16_t year)
{
    return ((year % 4) == 0 && (year % 100) != 0) || (year % 400) == 0;
}

static uint8_t days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31,
    };

    if (month == 2 && is_leap_year(year)) {
        return 29;
    }

    if (month < 1 || month > 12) {
        return 0;
    }

    return days[month - 1];
}

static uint8_t weekday_for_date(uint16_t year, uint8_t month, uint8_t day)
{
    if (month < 3) {
        month += 12;
        year--;
    }

    const uint16_t k = year % 100;
    const uint16_t j = year / 100;
    const uint16_t h = (uint16_t)(day + ((13 * (month + 1)) / 5) + k + (k / 4) + (j / 4) + (5 * j)) % 7;
    return (uint8_t)((h + 6) % 7);
}

bool rtc_pcf85063_datetime_is_valid(const rtc_datetime_t *datetime)
{
    if (datetime == NULL ||
        datetime->year < 2000 ||
        datetime->year > 2099 ||
        datetime->month < 1 ||
        datetime->month > 12 ||
        datetime->day < 1 ||
        datetime->day > days_in_month(datetime->year, datetime->month) ||
        datetime->weekday > 6 ||
        datetime->hour > 23 ||
        datetime->minute > 59 ||
        datetime->second > 59) {
        return false;
    }

    return true;
}

esp_err_t rtc_pcf85063_init_device(rtc_pcf85063_t *device,
                                   const char *bus,
                                   uint8_t address)
{
    if (device == NULL || bus == NULL || bus[0] == '\0' ||
        strnlen(bus, sizeof(device->bus)) >= sizeof(device->bus) ||
        address > 0x7fU) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    strlcpy(device->bus, bus, sizeof(device->bus));
    device->address = address;

    uint8_t ram = 0;
    esp_err_t ret = solar_os_bus_i2c_read_reg(device->bus,
                                              device->address,
                                              PCF85063_RAM_REG,
                                              &ram,
                                              1);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t ctrl1 = 0;
    ret = solar_os_bus_i2c_read_reg(device->bus,
                                    device->address,
                                    PCF85063_CTRL1_REG,
                                    &ctrl1,
                                    1);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint8_t updated = (uint8_t)(ctrl1 & ~(PCF85063_CTRL1_STOP_BIT | PCF85063_CTRL1_12H_BIT));
    if (updated == ctrl1) {
        return ESP_OK;
    }

    return solar_os_bus_i2c_write_reg(device->bus,
                                      device->address,
                                      PCF85063_CTRL1_REG,
                                      &updated,
                                      1);
}

esp_err_t rtc_pcf85063_get_datetime_device(const rtc_pcf85063_t *device,
                                           rtc_datetime_t *datetime)
{
    if (device == NULL || device->bus[0] == '\0' || datetime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[7];
    const esp_err_t ret = solar_os_bus_i2c_read_reg(device->bus,
                                                    device->address,
                                                    PCF85063_SEC_REG,
                                                    data,
                                                    sizeof(data));
    if (ret != ESP_OK) {
        return ret;
    }

    datetime->clock_integrity = (data[0] & PCF85063_SECONDS_OS_BIT) == 0;
    datetime->second = bcd_to_dec(data[0] & 0x7f);
    datetime->minute = bcd_to_dec(data[1] & 0x7f);
    datetime->hour = bcd_to_dec(data[2] & 0x3f);
    datetime->day = bcd_to_dec(data[3] & 0x3f);
    datetime->weekday = bcd_to_dec(data[4] & 0x07);
    datetime->month = bcd_to_dec(data[5] & 0x1f);
    datetime->year = (uint16_t)(2000 + bcd_to_dec(data[6]));

    if (!rtc_pcf85063_datetime_is_valid(datetime)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

esp_err_t rtc_pcf85063_set_datetime_device(const rtc_pcf85063_t *device,
                                           const rtc_datetime_t *datetime)
{
    if (device == NULL || device->bus[0] == '\0' ||
        !rtc_pcf85063_datetime_is_valid(datetime)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[7] = {
        dec_to_bcd(datetime->second),
        dec_to_bcd(datetime->minute),
        dec_to_bcd(datetime->hour),
        dec_to_bcd(datetime->day),
        dec_to_bcd(weekday_for_date(datetime->year, datetime->month, datetime->day)),
        dec_to_bcd(datetime->month),
        dec_to_bcd((uint8_t)(datetime->year % 100)),
    };

    rtc_pcf85063_t refreshed;
    esp_err_t ret = rtc_pcf85063_init_device(&refreshed,
                                             device->bus,
                                             device->address);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = solar_os_bus_i2c_write_reg(device->bus,
                                     device->address,
                                     PCF85063_SEC_REG,
                                     data,
                                     sizeof(data));
    if (ret != ESP_OK) {
        return ret;
    }

    return rtc_pcf85063_init_device(&refreshed,
                                    device->bus,
                                    device->address);
}

esp_err_t rtc_pcf85063_init(void)
{
    return rtc_pcf85063_init_device(&default_device,
                                    "i2c0",
                                    RTC_PCF85063_ADDRESS);
}

esp_err_t rtc_pcf85063_get_datetime(rtc_datetime_t *datetime)
{
    return rtc_pcf85063_get_datetime_device(&default_device, datetime);
}

esp_err_t rtc_pcf85063_set_datetime(const rtc_datetime_t *datetime)
{
    return rtc_pcf85063_set_datetime_device(&default_device, datetime);
}
