#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtc_pcf85063.h"

typedef struct {
    uint8_t reg;
    uint8_t data[8];
    size_t len;
} write_record_t;

static uint8_t registers[0x12];
static write_record_t writes[16];
static size_t write_count;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t src_len = strlen(src);
    if (size > 0) {
        const size_t copy_len = src_len < size - 1 ? src_len : size - 1;
        memcpy(dst, src, copy_len);
        dst[copy_len] = '\0';
    }
    return src_len;
}

esp_err_t solar_os_bus_i2c_read_reg(const char *name,
                                    uint8_t address,
                                    uint8_t reg,
                                    uint8_t *data,
                                    size_t len)
{
    assert(strcmp(name, "i2c0") == 0);
    assert(address == RTC_PCF85063_ADDRESS);
    assert((size_t)reg + len <= sizeof(registers));
    memcpy(data, &registers[reg], len);
    return ESP_OK;
}

esp_err_t solar_os_bus_i2c_write_reg(const char *name,
                                     uint8_t address,
                                     uint8_t reg,
                                     const uint8_t *data,
                                     size_t len)
{
    assert(strcmp(name, "i2c0") == 0);
    assert(address == RTC_PCF85063_ADDRESS);
    assert((size_t)reg + len <= sizeof(registers));
    assert(write_count < sizeof(writes) / sizeof(writes[0]));
    writes[write_count].reg = reg;
    writes[write_count].len = len;
    memcpy(writes[write_count].data, data, len);
    write_count++;
    memcpy(&registers[reg], data, len);
    return ESP_OK;
}

static void reset_bus(void)
{
    memset(registers, 0, sizeof(registers));
    memset(writes, 0, sizeof(writes));
    write_count = 0;
}

int main(void)
{
    const rtc_pcf85063_t device = {
        .bus = "i2c0",
        .address = RTC_PCF85063_ADDRESS,
    };

    reset_bus();
    registers[0x01] = 0xc8;
    const rtc_pcf85063_alarm_t alarm = {
        .match_fields = RTC_PCF85063_ALARM_MATCH_SECOND |
                        RTC_PCF85063_ALARM_MATCH_MINUTE |
                        RTC_PCF85063_ALARM_MATCH_HOUR,
        .second = 5,
        .minute = 42,
        .hour = 7,
    };
    assert(rtc_pcf85063_set_alarm_device(&device, &alarm) == ESP_OK);
    assert(write_count == 3);
    assert(writes[0].reg == 0x01 && writes[0].data[0] == 0x48);
    assert(writes[1].reg == 0x0b && writes[1].len == 5);
    assert(writes[1].data[0] == 0x05);
    assert(writes[1].data[1] == 0x42);
    assert(writes[1].data[2] == 0x07);
    assert(writes[1].data[3] == 0x80);
    assert(writes[1].data[4] == 0x80);
    assert(writes[2].reg == 0x01 && writes[2].data[0] == 0x88);

    reset_bus();
    assert(rtc_pcf85063_set_countdown_device(&device, 90, true) == ESP_OK);
    assert(write_count == 4);
    assert(writes[1].reg == 0x10 && writes[1].data[0] == 90);
    assert(writes[3].reg == 0x11 && writes[3].data[0] == 0x17);

    reset_bus();
    assert(rtc_pcf85063_set_countdown_device(&device, 300, false) == ESP_OK);
    assert(writes[1].reg == 0x10 && writes[1].data[0] == 5);
    assert(writes[3].reg == 0x11 && writes[3].data[0] == 0x1e);
    assert(rtc_pcf85063_set_countdown_device(&device, 256, false) ==
           ESP_ERR_INVALID_ARG);

    reset_bus();
    registers[0x01] = 0x48;
    uint32_t interrupts = 0;
    assert(rtc_pcf85063_get_interrupt_status_device(&device, &interrupts) == ESP_OK);
    assert(interrupts == (RTC_PCF85063_INTERRUPT_ALARM |
                          RTC_PCF85063_INTERRUPT_COUNTDOWN));
    assert(rtc_pcf85063_clear_interrupt_status_device(
        &device, RTC_PCF85063_INTERRUPT_ALARM) == ESP_OK);
    assert(writes[0].reg == 0x01 && writes[0].data[0] == 0x08);

    puts("pcf85063 rtc tests passed");
    return 0;
}
