#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtc_rx8130.h"

typedef struct {
    uint8_t registers[0x40];
    unsigned writes;
    bool fail_alarm_write;
} fake_io_t;

static esp_err_t fake_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    fake_io_t *fake = ctx;
    memcpy(data, &fake->registers[reg], len);
    return ESP_OK;
}

static esp_err_t fake_write(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    fake_io_t *fake = ctx;
    fake->writes++;
    if (reg == 0x17U) {
        assert((fake->registers[0x1E] & 0x08U) == 0U);
        if (fake->fail_alarm_write) return ESP_FAIL;
    }
    if (reg == 0x1DU && len == 1U) {
        fake->registers[reg] &= data[0]; /* Flags are write-0-to-clear. */
        return ESP_OK;
    }
    memcpy(&fake->registers[reg], data, len);
    return ESP_OK;
}

static void test_alarms(rtc_rx8130_t *device, fake_io_t *fake)
{
    fake->registers[0x1C] = 0x33U; /* Preserve the running timer and update mode. */
    fake->registers[0x1D] = 0x1AU; /* Pending AF, TF and VLF. */
    fake->registers[0x1E] = 0x30U; /* Preserve TIE and UIE. */
    rtc_rx8130_alarm_t alarm = {
        .match_fields = RTC_RX8130_ALARM_MATCH_MINUTE | RTC_RX8130_ALARM_MATCH_HOUR |
                        RTC_RX8130_ALARM_MATCH_DAY,
        .minute = 30, .hour = 12, .day = 15,
    };
    assert(rtc_rx8130_set_alarm(device, &alarm) == ESP_OK);
    assert(fake->registers[0x17] == 0x30U && fake->registers[0x18] == 0x12U);
    assert(fake->registers[0x19] == 0x15U);
    assert(fake->registers[0x1C] == 0x3BU); /* WADA selects the day-of-month. */
    assert(fake->registers[0x1D] == 0x12U); /* Only AF is cleared before arming. */
    assert(fake->registers[0x1E] == 0x38U);

    alarm.day = 31;
    assert(rtc_rx8130_set_alarm(device, &alarm) == ESP_OK);
    assert(fake->registers[0x19] == 0x31U && fake->registers[0x1C] == 0x3BU);
    alarm.match_fields = RTC_RX8130_ALARM_MATCH_HOUR | RTC_RX8130_ALARM_MATCH_WEEKDAY;
    alarm.hour = 6;
    alarm.weekday = 6;
    fake->registers[0x1D] |= 0x08U;
    assert(rtc_rx8130_set_alarm(device, &alarm) == ESP_OK);
    assert(fake->registers[0x17] == 0x80U && fake->registers[0x18] == 0x06U);
    assert(fake->registers[0x19] == 0x40U);
    assert(fake->registers[0x1C] == 0x33U); /* Clear WADA for the one-hot weekday. */
    assert(fake->registers[0x1D] == 0x12U && fake->registers[0x1E] == 0x38U);

    alarm.match_fields = RTC_RX8130_ALARM_MATCH_MINUTE;
    alarm.minute = 45;
    assert(rtc_rx8130_set_alarm(device, &alarm) == ESP_OK);
    assert(fake->registers[0x17] == 0x45U && fake->registers[0x18] == 0x80U);
    assert(fake->registers[0x19] == 0x80U);

    alarm.match_fields = RTC_RX8130_ALARM_MATCH_DAY;
    alarm.day = 0;
    const unsigned writes = fake->writes;
    assert(rtc_rx8130_set_alarm(device, &alarm) == ESP_ERR_INVALID_ARG);
    assert(fake->writes == writes);
    alarm.day = 15;
    fake->fail_alarm_write = true;
    assert(rtc_rx8130_set_alarm(device, &alarm) == ESP_FAIL);
    assert((fake->registers[0x1E] & 0x08U) == 0U);
    fake->fail_alarm_write = false;
    assert(rtc_rx8130_set_alarm(device, &alarm) == ESP_OK);
    assert(fake->registers[0x1E] == 0x38U);
}

int main(void)
{
    fake_io_t fake = {0};
    const rtc_rx8130_io_t io = {
        .read = fake_read,
        .write = fake_write,
        .ctx = &fake,
    };
    rtc_rx8130_t device;
    assert(rtc_rx8130_init(&device, &io) == ESP_OK);
    assert((fake.registers[0x1F] & 0x10U) != 0U);
    assert((fake.registers[0x1F] & 0x20U) == 0U);

    rtc_rx8130_datetime_t unset = {0};
    assert(rtc_rx8130_get_datetime(&device, &unset) == ESP_ERR_INVALID_RESPONSE);

    const rtc_rx8130_datetime_t set = {
        .year = 2026,
        .month = 11,
        .day = 11,
        .hour = 12,
        .minute = 30,
        .second = 0,
        .weekday = 3,
        .clock_integrity = true,
    };
    assert(rtc_rx8130_set_datetime(&device, &set) == ESP_OK);
    rtc_rx8130_datetime_t got = {0};
    assert(rtc_rx8130_get_datetime(&device, &got) == ESP_OK);
    assert(got.year == 2026 && got.month == 11 && got.day == 11);
    assert(got.hour == 12 && got.minute == 30 && got.weekday == 3);
    assert(got.clock_integrity);

    test_alarms(&device, &fake);
    assert(rtc_rx8130_deinit(&device) == ESP_OK);
    puts("ok");
    return 0;
}
