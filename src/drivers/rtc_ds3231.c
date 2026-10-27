#include "rtc_ds3231.h"

#include <string.h>
#include "solar_os_buses.h"

/* Analog Devices DS3231 Rev. 10, calendar and alarm register map. */
#define SECONDS 0x00U
#define ALARM1 0x07U
#define CONTROL 0x0eU
#define STATUS 0x0fU
#define EOSC 0x80U
#define INTCN 0x04U
#define A2IE 0x02U
#define A1IE 0x01U
#define OSF 0x80U
#define EN32KHZ 0x08U
#define A2F 0x02U
#define A1F 0x01U
#define FLAGS (OSF | A2F | A1F)
#define TIME_FIELDS (SOLAR_OS_DS3231_ALARM_MATCH_SECOND | \
                     SOLAR_OS_DS3231_ALARM_MATCH_MINUTE | \
                     SOLAR_OS_DS3231_ALARM_MATCH_HOUR)
#define DATE_FIELDS (SOLAR_OS_DS3231_ALARM_MATCH_DAY | SOLAR_OS_DS3231_ALARM_MATCH_WEEKDAY)

static bool valid_device(const solar_os_ds3231_t *device)
{
    return device && device->bus[0] && device->address == SOLAR_OS_DS3231_ADDRESS;
}

static esp_err_t read_reg(const solar_os_ds3231_t *d, uint8_t reg, uint8_t *data, size_t len)
{
    return solar_os_bus_i2c_read_reg(d->bus, d->address, reg, data, len);
}

static esp_err_t write_reg(const solar_os_ds3231_t *d, uint8_t reg, const uint8_t *data, size_t len)
{
    return solar_os_bus_i2c_write_reg(d->bus, d->address, reg, data, len);
}

static esp_err_t update_control(const solar_os_ds3231_t *d, uint8_t set, uint8_t clear)
{
    uint8_t value;
    esp_err_t err = read_reg(d, CONTROL, &value, 1);
    if (err != ESP_OK) return err;
    value = (uint8_t)((value | set) & ~clear);
    return write_reg(d, CONTROL, &value, 1);
}

static esp_err_t clear_flags(const solar_os_ds3231_t *d, uint8_t clear)
{
    uint8_t value;
    esp_err_t err = read_reg(d, STATUS, &value, 1);
    if (err != ESP_OK) return err;
    /* OSF/A1F/A2F are W0C. Ones preserve even events arriving after the read.
     * Preserve EN32kHz; reserved and read-only BSY bits are written as zero. */
    value = (uint8_t)(((value & EN32KHZ) | FLAGS) & ~clear);
    return write_reg(d, STATUS, &value, 1);
}

static uint8_t bcd(uint8_t n) { return (uint8_t)((n / 10U) * 16U + n % 10U); }
static uint8_t decimal(uint8_t n) { return (uint8_t)((n >> 4) * 10U + (n & 15U)); }
static bool valid_bcd(uint8_t n) { return (n & 15U) <= 9 && (n >> 4) <= 9; }

static bool valid_datetime(const solar_os_ds3231_datetime_t *dt)
{
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!dt || dt->year < 2000 || dt->year > 2099 || dt->month < 1 || dt->month > 12 ||
        dt->weekday > 6 || dt->hour > 23 || dt->minute > 59 || dt->second > 59)
        return false;
    const unsigned max_day = days[dt->month - 1] +
        (dt->month == 2 && dt->year % 4 == 0 ? 1U : 0U);
    return dt->day >= 1 && dt->day <= max_day;
}

static uint8_t weekday(const solar_os_ds3231_datetime_t *dt)
{
    static const unsigned offsets[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    unsigned year = dt->year - (dt->month < 3 ? 1U : 0U);
    return (uint8_t)((year + year/4 - year/100 + year/400 + offsets[dt->month-1] + dt->day) % 7);
}

esp_err_t solar_os_ds3231_init_device(solar_os_ds3231_t *device, const char *bus, uint8_t address)
{
    if (!device || !bus || !bus[0] || strnlen(bus, sizeof(device->bus)) >= sizeof(device->bus) ||
        address != SOLAR_OS_DS3231_ADDRESS) return ESP_ERR_INVALID_ARG;
    solar_os_ds3231_t candidate = {.address = address};
    strlcpy(candidate.bus, bus, sizeof(candidate.bus));
    uint8_t control;
    esp_err_t err = read_reg(&candidate, CONTROL, &control, 1);
    if (err != ESP_OK) return err;
    if (control & EOSC) {
        candidate.clock_integrity_lost = true;
        control &= (uint8_t)~EOSC;
        err = write_reg(&candidate, CONTROL, &control, 1);
        if (err != ESP_OK) return err;
    }
    /* Do not erase battery-backed calendar, OSF, aging trim or output settings. */
    *device = candidate;
    return ESP_OK;
}

esp_err_t solar_os_ds3231_get_datetime_device(const solar_os_ds3231_t *device,
                                             solar_os_ds3231_datetime_t *datetime)
{
    if (!valid_device(device) || !datetime) return ESP_ERR_INVALID_ARG;
    /* START latches the calendar into user buffers; one burst is rollover-safe. */
    uint8_t data[16];
    esp_err_t err = read_reg(device, SECONDS, data, sizeof(data));
    if (err != ESP_OK) return err;
    if ((data[0] & 0x80U) || (data[1] & 0x80U) || (data[2] & 0x80U) ||
        (data[3] & 0xf8U) || (data[4] & 0xc0U) || (data[5] & 0x60U))
        return ESP_ERR_INVALID_RESPONSE;
    const uint8_t hour = data[2] & ((data[2] & 0x40U) ? 0x1fU : 0x3fU);
    const uint8_t values[] = {data[0], data[1], hour, data[4], data[5] & 0x1fU, data[6]};
    for (size_t i = 0; i < sizeof(values); ++i)
        if (!valid_bcd(values[i])) return ESP_ERR_INVALID_RESPONSE;
    if (data[5] & 0x80U) return ESP_ERR_NOT_SUPPORTED;
    if (data[3] < 1 || data[3] > 7) return ESP_ERR_INVALID_RESPONSE;
    unsigned decoded_hour = decimal(hour);
    if (data[2] & 0x40U) {
        if (decoded_hour < 1 || decoded_hour > 12) return ESP_ERR_INVALID_RESPONSE;
        decoded_hour = decoded_hour % 12 + ((data[2] & 0x20U) ? 12U : 0U);
    }
    const solar_os_ds3231_datetime_t value = {
        .year = (uint16_t)(2000 + decimal(data[6])), .month = decimal(values[4]),
        .day = decimal(data[4]), .weekday = data[3] - 1,
        .hour = (uint8_t)decoded_hour, .minute = decimal(data[1]), .second = decimal(data[0]),
        .clock_integrity = !device->clock_integrity_lost && !(data[STATUS] & OSF) &&
            !(data[CONTROL] & EOSC),
    };
    if (!valid_datetime(&value)) return ESP_ERR_INVALID_RESPONSE;
    *datetime = value;
    return ESP_OK;
}

esp_err_t solar_os_ds3231_set_datetime_device(solar_os_ds3231_t *device,
                                             const solar_os_ds3231_datetime_t *datetime)
{
    if (!valid_device(device) || !valid_datetime(datetime)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = update_control(device, 0, EOSC);
    if (err != ESP_OK) return err;
    const uint8_t data[] = {bcd(datetime->second), bcd(datetime->minute), bcd(datetime->hour),
        weekday(datetime) + 1, bcd(datetime->day), bcd(datetime->month),
        bcd((uint8_t)(datetime->year - 2000))};
    /* A failed transfer may have written part of the calendar. Trust it again
     * only after a complete write and successful OSF acknowledgement. */
    device->clock_integrity_lost = true;
    err = write_reg(device, SECONDS, data, sizeof(data));
    if (err == ESP_OK) err = clear_flags(device, OSF);
    if (err == ESP_OK) device->clock_integrity_lost = false;
    return err;
}

esp_err_t solar_os_ds3231_set_alarm_device(const solar_os_ds3231_t *device,
                                          const solar_os_ds3231_alarm_t *alarm)
{
    if (!valid_device(device) || !alarm || !alarm->match_fields ||
        (alarm->match_fields & ~(TIME_FIELDS | DATE_FIELDS))) return ESP_ERR_INVALID_ARG;
    const uint32_t fields = alarm->match_fields;
    if (((fields & SOLAR_OS_DS3231_ALARM_MATCH_SECOND) && alarm->second > 59) ||
        ((fields & SOLAR_OS_DS3231_ALARM_MATCH_MINUTE) && alarm->minute > 59) ||
        ((fields & SOLAR_OS_DS3231_ALARM_MATCH_HOUR) && alarm->hour > 23) ||
        ((fields & SOLAR_OS_DS3231_ALARM_MATCH_DAY) && (alarm->day < 1 || alarm->day > 31)) ||
        ((fields & SOLAR_OS_DS3231_ALARM_MATCH_WEEKDAY) && alarm->weekday > 6))
        return ESP_ERR_INVALID_ARG;
    /* Only the mask patterns in datasheet Table 2 have defined behavior. */
    if (fields != SOLAR_OS_DS3231_ALARM_MATCH_SECOND &&
        fields != (SOLAR_OS_DS3231_ALARM_MATCH_SECOND | SOLAR_OS_DS3231_ALARM_MATCH_MINUTE) &&
        fields != TIME_FIELDS && fields != (TIME_FIELDS | SOLAR_OS_DS3231_ALARM_MATCH_DAY) &&
        fields != (TIME_FIELDS | SOLAR_OS_DS3231_ALARM_MATCH_WEEKDAY))
        return ESP_ERR_NOT_SUPPORTED;
    const uint8_t data[] = {bcd(alarm->second),
        fields & SOLAR_OS_DS3231_ALARM_MATCH_MINUTE ? bcd(alarm->minute) : 0x80U,
        fields & SOLAR_OS_DS3231_ALARM_MATCH_HOUR ? bcd(alarm->hour) : 0x80U,
        fields & SOLAR_OS_DS3231_ALARM_MATCH_DAY ? bcd(alarm->day) :
            (fields & SOLAR_OS_DS3231_ALARM_MATCH_WEEKDAY ? (uint8_t)(0x40U | (alarm->weekday + 1)) : 0x80U),
    };
    esp_err_t err = update_control(device, INTCN, A1IE | A2IE);
    if (err != ESP_OK) return err;
    err = write_reg(device, ALARM1, data, sizeof(data));
    if (err == ESP_OK) err = clear_flags(device, A1F | A2F);
    return err == ESP_OK ? update_control(device, A1IE | INTCN, A2IE) : err;
}

esp_err_t solar_os_ds3231_disable_alarm_device(const solar_os_ds3231_t *device)
{
    if (!valid_device(device)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = update_control(device, INTCN, A1IE | A2IE);
    return err == ESP_OK ? clear_flags(device, A1F | A2F) : err;
}

esp_err_t solar_os_ds3231_get_interrupt_status_device(const solar_os_ds3231_t *device,
                                                     uint32_t *interrupts)
{
    if (!valid_device(device) || !interrupts) return ESP_ERR_INVALID_ARG;
    uint8_t status;
    esp_err_t err = read_reg(device, STATUS, &status, 1);
    if (err == ESP_OK) *interrupts = (status & A1F) ? SOLAR_OS_DS3231_INTERRUPT_ALARM : 0;
    return err;
}

esp_err_t solar_os_ds3231_clear_interrupt_status_device(const solar_os_ds3231_t *device,
                                                       uint32_t interrupts)
{
    if (!valid_device(device) || (interrupts & ~SOLAR_OS_DS3231_INTERRUPT_ALARM))
        return ESP_ERR_INVALID_ARG;
    return interrupts ? clear_flags(device, A1F | A2F) : ESP_OK;
}
