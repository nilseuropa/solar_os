#include "rtc_pcf85063.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

#define PCF85063_CTRL1_REG 0x00
#define PCF85063_CTRL2_REG 0x01
#define PCF85063_RAM_REG 0x03
#define PCF85063_SEC_REG 0x04
#define PCF85063_ALARM_REG 0x0b
#define PCF85063_TIMER_VALUE_REG 0x10
#define PCF85063_TIMER_MODE_REG 0x11
#define PCF85063_CTRL1_EXT_TEST_BIT 0x80
#define PCF85063_CTRL1_RESERVED_BITS 0x48
#define PCF85063_CTRL1_SOFTWARE_RESET 0x58
#define PCF85063_CTRL1_STOP_BIT 0x20
#define PCF85063_CTRL1_12H_BIT 0x02
#define PCF85063_SECONDS_OS_BIT 0x80
#define PCF85063_ALARM_DISABLE_BIT 0x80
#define PCF85063_CTRL2_AIE_BIT 0x80
#define PCF85063_CTRL2_AF_BIT 0x40
#define PCF85063_CTRL2_TF_BIT 0x08
#define PCF85063_TIMER_TCF_1HZ 0x10
#define PCF85063_TIMER_TCF_1_60HZ 0x18
#define PCF85063_TIMER_TE_BIT 0x04
#define PCF85063_TIMER_TIE_BIT 0x02
#define PCF85063_TIMER_TP_BIT 0x01

#define PCF85063_ALARM_MATCH_ALL (RTC_PCF85063_ALARM_MATCH_SECOND | \
                                  RTC_PCF85063_ALARM_MATCH_MINUTE | \
                                  RTC_PCF85063_ALARM_MATCH_HOUR | \
                                  RTC_PCF85063_ALARM_MATCH_DAY | \
                                  RTC_PCF85063_ALARM_MATCH_WEEKDAY)
#define PCF85063_INTERRUPT_ALL (RTC_PCF85063_INTERRUPT_ALARM | \
                                RTC_PCF85063_INTERRUPT_COUNTDOWN)

static rtc_pcf85063_t default_device;

static uint8_t bcd_to_dec(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0f));
}

static uint8_t dec_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static bool device_is_valid(const rtc_pcf85063_t *device)
{
    return device != NULL && device->bus[0] != '\0' && device->address <= 0x7fU;
}

static esp_err_t update_control2(const rtc_pcf85063_t *device,
                                 uint8_t set_bits,
                                 uint8_t clear_bits)
{
    uint8_t control = 0;
    esp_err_t ret = solar_os_bus_i2c_read_reg(device->bus,
                                              device->address,
                                              PCF85063_CTRL2_REG,
                                              &control,
                                              1);
    if (ret != ESP_OK) {
        return ret;
    }
    control = (uint8_t)((control | set_bits) & ~clear_bits);
    return solar_os_bus_i2c_write_reg(device->bus,
                                      device->address,
                                      PCF85063_CTRL2_REG,
                                      &control,
                                      1);
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

    /* A PCF85063 can power up in an undefined state with EXT_TEST (bit 7,
     * external-clock test mode) or reserved bits set in Control_1. In that
     * state the oscillator is bypassed, the time registers never advance,
     * and their contents are junk - and merely clearing STOP below would
     * preserve the bad bits. The datasheet's remedy is a software reset,
     * which restores every register to its documented default. Only do it
     * when the register is provably invalid so a sane, running clock is
     * never disturbed. Seen in the wild on the LilyGO T-LoRa-Pager. */
    if ((ctrl1 & (PCF85063_CTRL1_EXT_TEST_BIT | PCF85063_CTRL1_RESERVED_BITS)) != 0U) {
        const uint8_t reset = PCF85063_CTRL1_SOFTWARE_RESET;
        ESP_LOGW("pcf85063", "Control_1 0x%02x is invalid (EXT_TEST/reserved); issuing software reset", ctrl1);
        ret = solar_os_bus_i2c_write_reg(device->bus, device->address, PCF85063_CTRL1_REG, &reset, 1);
        if (ret != ESP_OK) {
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        ret = solar_os_bus_i2c_read_reg(device->bus, device->address, PCF85063_CTRL1_REG, &ctrl1, 1);
        if (ret != ESP_OK) {
            return ret;
        }
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

esp_err_t rtc_pcf85063_set_alarm_device(const rtc_pcf85063_t *device,
                                        const rtc_pcf85063_alarm_t *alarm)
{
    if (!device_is_valid(device) || alarm == NULL || alarm->match_fields == 0 ||
        (alarm->match_fields & ~PCF85063_ALARM_MATCH_ALL) != 0 ||
        ((alarm->match_fields & RTC_PCF85063_ALARM_MATCH_SECOND) != 0 &&
         alarm->second > 59) ||
        ((alarm->match_fields & RTC_PCF85063_ALARM_MATCH_MINUTE) != 0 &&
         alarm->minute > 59) ||
        ((alarm->match_fields & RTC_PCF85063_ALARM_MATCH_HOUR) != 0 &&
         alarm->hour > 23) ||
        ((alarm->match_fields & RTC_PCF85063_ALARM_MATCH_DAY) != 0 &&
         (alarm->day < 1 || alarm->day > 31)) ||
        ((alarm->match_fields & RTC_PCF85063_ALARM_MATCH_WEEKDAY) != 0 &&
         alarm->weekday > 6)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t registers[5] = {
        PCF85063_ALARM_DISABLE_BIT,
        PCF85063_ALARM_DISABLE_BIT,
        PCF85063_ALARM_DISABLE_BIT,
        PCF85063_ALARM_DISABLE_BIT,
        PCF85063_ALARM_DISABLE_BIT,
    };
    if ((alarm->match_fields & RTC_PCF85063_ALARM_MATCH_SECOND) != 0) {
        registers[0] = dec_to_bcd(alarm->second);
    }
    if ((alarm->match_fields & RTC_PCF85063_ALARM_MATCH_MINUTE) != 0) {
        registers[1] = dec_to_bcd(alarm->minute);
    }
    if ((alarm->match_fields & RTC_PCF85063_ALARM_MATCH_HOUR) != 0) {
        registers[2] = dec_to_bcd(alarm->hour);
    }
    if ((alarm->match_fields & RTC_PCF85063_ALARM_MATCH_DAY) != 0) {
        registers[3] = dec_to_bcd(alarm->day);
    }
    if ((alarm->match_fields & RTC_PCF85063_ALARM_MATCH_WEEKDAY) != 0) {
        registers[4] = dec_to_bcd(alarm->weekday);
    }

    esp_err_t ret = update_control2(device, 0, PCF85063_CTRL2_AIE_BIT);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_bus_i2c_write_reg(device->bus,
                                     device->address,
                                     PCF85063_ALARM_REG,
                                     registers,
                                     sizeof(registers));
    if (ret != ESP_OK) {
        return ret;
    }
    return update_control2(device,
                           PCF85063_CTRL2_AIE_BIT,
                           PCF85063_CTRL2_AF_BIT);
}

esp_err_t rtc_pcf85063_disable_alarm_device(const rtc_pcf85063_t *device)
{
    if (!device_is_valid(device)) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t registers[5] = {
        PCF85063_ALARM_DISABLE_BIT,
        PCF85063_ALARM_DISABLE_BIT,
        PCF85063_ALARM_DISABLE_BIT,
        PCF85063_ALARM_DISABLE_BIT,
        PCF85063_ALARM_DISABLE_BIT,
    };
    esp_err_t ret = update_control2(device, 0, PCF85063_CTRL2_AIE_BIT);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_bus_i2c_write_reg(device->bus,
                                     device->address,
                                     PCF85063_ALARM_REG,
                                     registers,
                                     sizeof(registers));
    if (ret != ESP_OK) {
        return ret;
    }
    return update_control2(device,
                           0,
                           PCF85063_CTRL2_AIE_BIT | PCF85063_CTRL2_AF_BIT);
}

esp_err_t rtc_pcf85063_set_countdown_device(const rtc_pcf85063_t *device,
                                            uint32_t period_seconds,
                                            bool repeat)
{
    if (!device_is_valid(device) || period_seconds == 0 ||
        (period_seconds > 255 &&
         (period_seconds > 255U * 60U || period_seconds % 60U != 0))) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t timer_mode = 0;
    esp_err_t ret = solar_os_bus_i2c_read_reg(device->bus,
                                              device->address,
                                              PCF85063_TIMER_MODE_REG,
                                              &timer_mode,
                                              1);
    if (ret != ESP_OK) {
        return ret;
    }
    timer_mode = (uint8_t)(timer_mode &
        ~(PCF85063_TIMER_TE_BIT | PCF85063_TIMER_TIE_BIT | PCF85063_TIMER_TP_BIT));
    ret = solar_os_bus_i2c_write_reg(device->bus,
                                     device->address,
                                     PCF85063_TIMER_MODE_REG,
                                     &timer_mode,
                                     1);
    if (ret != ESP_OK) {
        return ret;
    }

    const bool use_minutes = period_seconds > 255;
    const uint8_t timer_value = (uint8_t)(use_minutes
        ? period_seconds / 60U
        : period_seconds);
    ret = solar_os_bus_i2c_write_reg(device->bus,
                                     device->address,
                                     PCF85063_TIMER_VALUE_REG,
                                     &timer_value,
                                     1);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = update_control2(device, 0, PCF85063_CTRL2_TF_BIT);
    if (ret != ESP_OK) {
        return ret;
    }

    timer_mode = (uint8_t)((use_minutes
        ? PCF85063_TIMER_TCF_1_60HZ
        : PCF85063_TIMER_TCF_1HZ) |
        PCF85063_TIMER_TE_BIT | PCF85063_TIMER_TIE_BIT |
        (repeat ? PCF85063_TIMER_TP_BIT : 0));
    return solar_os_bus_i2c_write_reg(device->bus,
                                      device->address,
                                      PCF85063_TIMER_MODE_REG,
                                      &timer_mode,
                                      1);
}

esp_err_t rtc_pcf85063_disable_countdown_device(const rtc_pcf85063_t *device)
{
    if (!device_is_valid(device)) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t timer_mode = PCF85063_TIMER_TCF_1_60HZ;
    esp_err_t ret = solar_os_bus_i2c_write_reg(device->bus,
                                               device->address,
                                               PCF85063_TIMER_MODE_REG,
                                               &timer_mode,
                                               1);
    if (ret != ESP_OK) {
        return ret;
    }
    const uint8_t timer_value = 0;
    ret = solar_os_bus_i2c_write_reg(device->bus,
                                     device->address,
                                     PCF85063_TIMER_VALUE_REG,
                                     &timer_value,
                                     1);
    if (ret != ESP_OK) {
        return ret;
    }
    return update_control2(device, 0, PCF85063_CTRL2_TF_BIT);
}

esp_err_t rtc_pcf85063_get_interrupt_status_device(const rtc_pcf85063_t *device,
                                                   uint32_t *interrupts)
{
    if (!device_is_valid(device) || interrupts == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t control = 0;
    const esp_err_t ret = solar_os_bus_i2c_read_reg(device->bus,
                                                    device->address,
                                                    PCF85063_CTRL2_REG,
                                                    &control,
                                                    1);
    if (ret != ESP_OK) {
        return ret;
    }
    *interrupts = 0;
    if ((control & PCF85063_CTRL2_AF_BIT) != 0) {
        *interrupts |= RTC_PCF85063_INTERRUPT_ALARM;
    }
    if ((control & PCF85063_CTRL2_TF_BIT) != 0) {
        *interrupts |= RTC_PCF85063_INTERRUPT_COUNTDOWN;
    }
    return ESP_OK;
}

esp_err_t rtc_pcf85063_clear_interrupt_status_device(const rtc_pcf85063_t *device,
                                                     uint32_t interrupts)
{
    if (!device_is_valid(device) || interrupts == 0 ||
        (interrupts & ~PCF85063_INTERRUPT_ALL) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t clear_bits = 0;
    if ((interrupts & RTC_PCF85063_INTERRUPT_ALARM) != 0) {
        clear_bits |= PCF85063_CTRL2_AF_BIT;
    }
    if ((interrupts & RTC_PCF85063_INTERRUPT_COUNTDOWN) != 0) {
        clear_bits |= PCF85063_CTRL2_TF_BIT;
    }
    return update_control2(device,
                           PCF85063_CTRL2_AF_BIT | PCF85063_CTRL2_TF_BIT,
                           clear_bits);
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
