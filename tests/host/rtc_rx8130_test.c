#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtc_rx8130.h"

typedef struct {
    uint8_t registers[0x40];
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
    memcpy(&fake->registers[reg], data, len);
    return ESP_OK;
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

    assert(rtc_rx8130_deinit(&device) == ESP_OK);
    puts("ok");
    return 0;
}
