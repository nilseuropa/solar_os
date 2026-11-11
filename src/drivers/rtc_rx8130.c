#include "rtc_rx8130.h"

#include <string.h>

#define REG_SEC 0x10U
#define REG_EXT 0x1CU
#define REG_FLAG 0x1DU
#define REG_CTRL0 0x1EU
#define REG_CTRL1 0x1FU
#define REG_ALARM_MIN 0x17U
#define REG_TIMER_CNT 0x1AU
#define REG_OFFSET 0x30U

#define FLAG_VLF 0x02U
#define EXT_TE 0x10U
#define CTRL0_AIE 0x08U
#define CTRL0_TIE 0x10U
#define AE_BIT 0x80U

static bool valid_device(const rtc_rx8130_t *device)
{
    return device != NULL && device->initialized &&
        device->io.read != NULL && device->io.write != NULL;
}

static uint8_t bcd_to_bin(uint8_t value)
{
    return (uint8_t)((value >> 4U) * 10U + (value & 0x0FU));
}

static uint8_t bin_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4U) | (value % 10U));
}

static bool valid_bcd(uint8_t value, uint8_t mask)
{
    const uint8_t v = (uint8_t)(value & mask);
    return ((v >> 4U) <= 9U) && ((v & 0x0FU) <= 9U);
}

static int weekday_from_bit(uint8_t value)
{
    if (value == 0U || (value & (uint8_t)(value - 1U)) != 0U) {
        return -1;
    }
    int bit = 0;
    while ((value & 1U) == 0U) {
        value >>= 1U;
        bit++;
    }
    return bit;
}

esp_err_t rtc_rx8130_init(rtc_rx8130_t *device, const rtc_rx8130_io_t *io)
{
    if (device == NULL || io == NULL || io->read == NULL || io->write == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    device->io = *io;
    uint8_t ctrl1 = 0U;
    esp_err_t ret = device->io.read(device->io.ctx, REG_CTRL1, &ctrl1, 1U);
    if (ret != ESP_OK) {
        return ret;
    }
    /* INIEN on, CHGEN off (Paper Mono uses a non-rechargeable backup). */
    ctrl1 = (uint8_t)((ctrl1 & (uint8_t)~0x20U) | 0x10U);
    const uint8_t zero = 0U;
    ret = device->io.write(device->io.ctx, REG_CTRL1, &ctrl1, 1U);
    if (ret == ESP_OK) {
        ret = device->io.write(device->io.ctx, REG_OFFSET, &zero, 1U);
    }
    if (ret == ESP_OK) {
        ret = device->io.write(device->io.ctx, REG_CTRL0, &zero, 1U);
    }
    if (ret == ESP_OK) {
        uint8_t ext = 0U;
        ret = device->io.read(device->io.ctx, REG_EXT, &ext, 1U);
        if (ret == ESP_OK) {
            ext = (uint8_t)(ext & (uint8_t)~EXT_TE);
            ret = device->io.write(device->io.ctx, REG_EXT, &ext, 1U);
        }
    }
    if (ret != ESP_OK) {
        return ret;
    }
    device->initialized = true;
    return ESP_OK;
}

esp_err_t rtc_rx8130_deinit(rtc_rx8130_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    return ESP_OK;
}

esp_err_t rtc_rx8130_get_datetime(rtc_rx8130_t *device, rtc_rx8130_datetime_t *datetime)
{
    if (!valid_device(device) || datetime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t raw[7] = {0};
    uint8_t flag = 0U;
    esp_err_t ret = device->io.read(device->io.ctx, REG_SEC, raw, sizeof(raw));
    if (ret == ESP_OK) {
        ret = device->io.read(device->io.ctx, REG_FLAG, &flag, 1U);
    }
    if (ret != ESP_OK) {
        return ret;
    }
    const uint8_t sec = (uint8_t)(raw[0] & 0x7FU);
    const uint8_t min = (uint8_t)(raw[1] & 0x7FU);
    const uint8_t hour = (uint8_t)(raw[2] & 0x3FU);
    const uint8_t day = (uint8_t)(raw[4] & 0x3FU);
    const uint8_t month = (uint8_t)(raw[5] & 0x1FU);
    const uint8_t year = raw[6];
    const int weekday = weekday_from_bit(raw[3]);
    if (weekday < 0 || !valid_bcd(sec, 0x7FU) || !valid_bcd(min, 0x7FU) ||
        !valid_bcd(hour, 0x3FU) || !valid_bcd(day, 0x3FU) ||
        !valid_bcd(month, 0x1FU) || !valid_bcd(year, 0xFFU)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint8_t month_n = bcd_to_bin(month);
    const uint8_t day_n = bcd_to_bin(day);
    const uint8_t hour_n = bcd_to_bin(hour);
    const uint8_t min_n = bcd_to_bin(min);
    const uint8_t sec_n = bcd_to_bin(sec);
    if (month_n < 1U || month_n > 12U || day_n < 1U || day_n > 31U ||
        hour_n > 23U || min_n > 59U || sec_n > 59U) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *datetime = (rtc_rx8130_datetime_t) {
        .year = (uint16_t)(2000U + bcd_to_bin(year)),
        .month = month_n,
        .day = day_n,
        .hour = hour_n,
        .minute = min_n,
        .second = sec_n,
        .weekday = (uint8_t)weekday,
        .clock_integrity = (flag & FLAG_VLF) == 0U,
    };
    if (!datetime->clock_integrity) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t rtc_rx8130_set_datetime(rtc_rx8130_t *device, const rtc_rx8130_datetime_t *datetime)
{
    if (!valid_device(device) || datetime == NULL ||
        datetime->year < 2000U || datetime->year > 2099U ||
        datetime->month < 1U || datetime->month > 12U ||
        datetime->day < 1U || datetime->day > 31U ||
        datetime->hour > 23U || datetime->minute > 59U ||
        datetime->second > 59U || datetime->weekday > 6U) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t raw[7] = {
        bin_to_bcd(datetime->second),
        bin_to_bcd(datetime->minute),
        bin_to_bcd(datetime->hour),
        (uint8_t)(1U << (datetime->weekday & 7U)),
        bin_to_bcd(datetime->day),
        bin_to_bcd(datetime->month),
        bin_to_bcd((uint8_t)(datetime->year % 100U)),
    };
    esp_err_t ret = device->io.write(device->io.ctx, REG_SEC, raw, sizeof(raw));
    if (ret == ESP_OK) {
        /* Clear VLF (write-0-to-clear). Leave other W0C flags alone with 1s. */
        const uint8_t clear = (uint8_t)~FLAG_VLF;
        ret = device->io.write(device->io.ctx, REG_FLAG, &clear, 1U);
    }
    return ret;
}

esp_err_t rtc_rx8130_set_alarm(rtc_rx8130_t *device, const rtc_rx8130_alarm_t *alarm)
{
    if (!valid_device(device) || alarm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Date takes precedence over weekday, matching the RX8130 hardware rule. */
    uint8_t min = AE_BIT;
    uint8_t hour = AE_BIT;
    uint8_t wday = AE_BIT;
    if ((alarm->match_fields & RTC_RX8130_ALARM_MATCH_MINUTE) != 0U) {
        if (alarm->minute > 59U) {
            return ESP_ERR_INVALID_ARG;
        }
        min = bin_to_bcd(alarm->minute);
    }
    if ((alarm->match_fields & RTC_RX8130_ALARM_MATCH_HOUR) != 0U) {
        if (alarm->hour > 23U) {
            return ESP_ERR_INVALID_ARG;
        }
        hour = bin_to_bcd(alarm->hour);
    }
    if ((alarm->match_fields & RTC_RX8130_ALARM_MATCH_DAY) != 0U) {
        if (alarm->day < 1U || alarm->day > 31U) {
            return ESP_ERR_INVALID_ARG;
        }
        wday = (uint8_t)(0x40U | bin_to_bcd(alarm->day));
    } else if ((alarm->match_fields & RTC_RX8130_ALARM_MATCH_WEEKDAY) != 0U) {
        if (alarm->weekday > 6U) {
            return ESP_ERR_INVALID_ARG;
        }
        wday = (uint8_t)(1U << (alarm->weekday & 7U));
    }
    const uint8_t raw[3] = {min, hour, wday};
    esp_err_t ret = device->io.write(device->io.ctx, REG_ALARM_MIN, raw, sizeof(raw));
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t ctrl0 = 0U;
    ret = device->io.read(device->io.ctx, REG_CTRL0, &ctrl0, 1U);
    if (ret != ESP_OK) {
        return ret;
    }
    ctrl0 = (uint8_t)(ctrl0 | CTRL0_AIE);
    return device->io.write(device->io.ctx, REG_CTRL0, &ctrl0, 1U);
}

esp_err_t rtc_rx8130_disable_alarm(rtc_rx8130_t *device)
{
    if (!valid_device(device)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t ctrl0 = 0U;
    esp_err_t ret = device->io.read(device->io.ctx, REG_CTRL0, &ctrl0, 1U);
    if (ret != ESP_OK) {
        return ret;
    }
    ctrl0 = (uint8_t)(ctrl0 & (uint8_t)~CTRL0_AIE);
    return device->io.write(device->io.ctx, REG_CTRL0, &ctrl0, 1U);
}

esp_err_t rtc_rx8130_set_countdown(rtc_rx8130_t *device, uint32_t period_seconds, bool repeat)
{
    if (!valid_device(device) || period_seconds == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tsel;
    uint16_t count;
    if (period_seconds <= 65535U) {
        tsel = 0x02U; /* 1 Hz */
        count = (uint16_t)period_seconds;
    } else if (period_seconds <= 65535U * 60U) {
        tsel = 0x03U; /* 1/60 Hz */
        count = (uint16_t)((period_seconds + 59U) / 60U);
    } else if (period_seconds <= 65535U * 3600U) {
        tsel = 0x04U; /* 1/3600 Hz */
        count = (uint16_t)((period_seconds + 3599U) / 3600U);
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    (void)repeat; /* RX8130 TE is a single-shot or periodic via EXT; keep TE armed. */
    uint8_t ext = 0U;
    esp_err_t ret = device->io.read(device->io.ctx, REG_EXT, &ext, 1U);
    if (ret != ESP_OK) {
        return ret;
    }
    ext = (uint8_t)((ext & (uint8_t)~0x17U) | tsel);
    ret = device->io.write(device->io.ctx, REG_EXT, &ext, 1U);
    if (ret != ESP_OK) {
        return ret;
    }
    const uint8_t cnt[2] = {(uint8_t)(count & 0xFFU), (uint8_t)(count >> 8U)};
    ret = device->io.write(device->io.ctx, REG_TIMER_CNT, cnt, sizeof(cnt));
    if (ret != ESP_OK) {
        return ret;
    }
    ext = (uint8_t)(ext | EXT_TE);
    ret = device->io.write(device->io.ctx, REG_EXT, &ext, 1U);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t ctrl0 = 0U;
    ret = device->io.read(device->io.ctx, REG_CTRL0, &ctrl0, 1U);
    if (ret != ESP_OK) {
        return ret;
    }
    ctrl0 = (uint8_t)(ctrl0 | CTRL0_TIE);
    return device->io.write(device->io.ctx, REG_CTRL0, &ctrl0, 1U);
}

esp_err_t rtc_rx8130_disable_countdown(rtc_rx8130_t *device)
{
    if (!valid_device(device)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t ext = 0U;
    esp_err_t ret = device->io.read(device->io.ctx, REG_EXT, &ext, 1U);
    if (ret != ESP_OK) {
        return ret;
    }
    ext = (uint8_t)(ext & (uint8_t)~EXT_TE);
    ret = device->io.write(device->io.ctx, REG_EXT, &ext, 1U);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t ctrl0 = 0U;
    ret = device->io.read(device->io.ctx, REG_CTRL0, &ctrl0, 1U);
    if (ret != ESP_OK) {
        return ret;
    }
    ctrl0 = (uint8_t)(ctrl0 & (uint8_t)~CTRL0_TIE);
    return device->io.write(device->io.ctx, REG_CTRL0, &ctrl0, 1U);
}
