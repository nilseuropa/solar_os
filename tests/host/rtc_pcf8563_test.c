#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rtc_pcf8563.h"
#include "solar_os_pcf8563.h"
#include "solar_os_rtc.h"

static uint8_t registers[16];
static int fail_read = -1, fail_write = -1;
static unsigned writes;
static bool timer_race;
static solar_os_rtc_provider_t provider;
static esp_err_t provider_error;
static unsigned warning_logs, error_logs;
static char last_log[192];

void rtc_pcf8563_test_log(bool error, const char *tag, const char *format, ...)
{
    assert(strcmp(tag, "pcf8563") == 0);
    if (error) ++error_logs;
    else ++warning_logs;
    va_list args;
    va_start(args, format);
    vsnprintf(last_log, sizeof(last_log), format, args);
    va_end(args);
}

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    default: return "unknown error";
    }
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) { size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n); dst[n] = 0; }
    return len;
}

esp_err_t solar_os_bus_i2c_read_reg(const char *bus, uint8_t addr,
    uint8_t reg, uint8_t *data, size_t len)
{
    assert(strcmp(bus, "i2c0") == 0 && addr == 0x51);
    assert((size_t)reg + len <= sizeof(registers));
    if (reg == fail_read) return ESP_ERR_TIMEOUT;
    memcpy(data, registers + reg, len);
    /* Inject a timer event after Control_2 was read, before acknowledgement. */
    if (reg == 1 && timer_race) { registers[1] |= 4; timer_race = false; }
    return ESP_OK;
}

esp_err_t solar_os_bus_i2c_write_reg(const char *bus, uint8_t addr,
    uint8_t reg, const uint8_t *data, size_t len)
{
    assert(strcmp(bus, "i2c0") == 0 && addr == 0x51);
    assert((size_t)reg + len <= sizeof(registers));
    if (reg == fail_write) return ESP_ERR_TIMEOUT;
    ++writes;
    for (size_t i = 0; i < len; ++i) {
        if (reg + i == 1) {
            /* AF/TF are W0C, not ordinary memory. */
            registers[1] = (uint8_t)((data[i] & ~0x0cU) |
                (registers[1] & data[i] & 0x0cU));
        } else registers[reg + i] = data[i];
    }
    return ESP_OK;
}

esp_err_t solar_os_rtc_register_provider(const char *name, const solar_os_rtc_provider_t *value)
{
    assert(strcmp(name, "rtc0") == 0);
    if (provider_error != ESP_OK) return provider_error;
    provider = *value;
    return ESP_OK;
}

esp_err_t solar_os_rtc_unregister_provider(const char *name)
{
    assert(strcmp(name, "rtc0") == 0);
    if (provider_error != ESP_OK) return provider_error;
    memset(&provider, 0, sizeof(provider));
    return ESP_OK;
}

int main(void)
{
    rtc_pcf8563_t chip;
    rtc_pcf8563_datetime_t value;
    const uint8_t calendar[] = {0x85, 0x42, 0x07, 0x29, 0x04, 0x02, 0x24};
    memcpy(registers + 2, calendar, sizeof(calendar));
    registers[0] = 0xa8; registers[1] = 0x1f; registers[13] = 0x83;
    assert(rtc_pcf8563_init_device(&chip, "i2c0", 0x51) == ESP_OK);
    assert(writes == 1 && registers[0] == 0);
    assert(registers[1] == 0x1f && registers[2] == 0x85 && registers[13] == 0x83);
    assert(chip.clock_integrity_lost);
    assert(rtc_pcf8563_get_datetime_device(&chip, &value) == ESP_OK);
    assert(value.year == 2024 && value.month == 2 && value.day == 29 &&
           value.hour == 7 && value.minute == 42 && value.second == 5 && !value.clock_integrity);
    registers[2] = 5;
    assert(rtc_pcf8563_get_datetime_device(&chip, &value) == ESP_OK && !value.clock_integrity);
    assert(rtc_pcf8563_set_datetime_device(&chip, &value) == ESP_OK);
    assert(rtc_pcf8563_get_datetime_device(&chip, &value) == ESP_OK && value.clock_integrity);
    registers[0] = 0x20;
    assert(rtc_pcf8563_get_datetime_device(&chip, &value) == ESP_OK && !value.clock_integrity);
    registers[0] = 0;
    registers[8] = 0x23;
    assert(rtc_pcf8563_get_datetime_device(&chip, &value) == ESP_ERR_INVALID_RESPONSE);
    registers[8] = 0x24; registers[3] = 0x1a;
    assert(rtc_pcf8563_get_datetime_device(&chip, &value) == ESP_ERR_INVALID_RESPONSE);
    registers[3] = 0x42; registers[7] = 0x82;
    assert(rtc_pcf8563_get_datetime_device(&chip, &value) == ESP_ERR_NOT_SUPPORTED);
    registers[7] = 2;
    value = (rtc_pcf8563_datetime_t){.year=2026, .month=10, .day=3, .hour=12, .minute=34, .second=56};
    assert(rtc_pcf8563_set_datetime_device(&chip, &value) == ESP_OK);
    assert(registers[2] == 0x56 && registers[3] == 0x34 && registers[4] == 0x12 &&
           registers[5] == 3 && registers[6] == 6 && registers[7] == 0x10 && registers[8] == 0x26);
    assert(registers[0] == 0 && registers[13] == 0x83);
    value.year = 2100;
    assert(rtc_pcf8563_set_datetime_device(&chip, &value) == ESP_ERR_INVALID_ARG);
    value.year = 2026;
    fail_write = 2;
    assert(rtc_pcf8563_set_datetime_device(&chip, &value) == ESP_ERR_TIMEOUT);
    assert(registers[0] == 0); /* Resume even after the date transfer fails. */
    assert(chip.clock_integrity_lost);
    fail_write = -1; fail_read = 0;
    assert(rtc_pcf8563_get_datetime_device(&chip, &value) == ESP_ERR_TIMEOUT);
    fail_read = -1;

    rtc_pcf8563_alarm_t alarm = {.match_fields=RTC_PCF8563_ALARM_MATCH_SECOND |
        RTC_PCF8563_ALARM_MATCH_MINUTE | RTC_PCF8563_ALARM_MATCH_HOUR,
        .minute=42, .hour=7};
    registers[1] = 0x0d; /* AF, TF, TIE */
    assert(rtc_pcf8563_set_alarm_device(&chip, &alarm) == ESP_OK);
    assert(registers[9] == 0x42 && registers[10] == 7 && registers[11] == 0x80 && registers[12] == 0x80);
    assert(registers[1] == 7); /* TIE/TF preserved, AIE set, AF cleared. */
    unsigned before = writes;
    alarm.second = 1;
    assert(rtc_pcf8563_set_alarm_device(&chip, &alarm) == ESP_ERR_NOT_SUPPORTED && writes == before);
    alarm.second = 0; alarm.minute = 60;
    assert(rtc_pcf8563_set_alarm_device(&chip, &alarm) == ESP_ERR_INVALID_ARG && writes == before);
    alarm.minute = 42; alarm.match_fields = RTC_PCF8563_ALARM_MATCH_SECOND;
    assert(rtc_pcf8563_set_alarm_device(&chip, &alarm) == ESP_ERR_NOT_SUPPORTED);
    registers[1] = 0x0b; timer_race = true;
    assert(rtc_pcf8563_clear_interrupt_status_device(&chip, RTC_PCF8563_INTERRUPT_ALARM) == ESP_OK);
    assert(registers[1] == 7); /* Timer event arriving during acknowledgement survives. */
    assert(rtc_pcf8563_disable_alarm_device(&chip) == ESP_OK);
    assert(registers[1] == 5 && registers[9] == 0x80);

    assert(rtc_pcf8563_set_countdown_device(&chip, 255, false) == ESP_OK);
    assert(registers[14] == 0x82 && registers[15] == 255 && registers[1] == 1);
    assert(rtc_pcf8563_set_countdown_device(&chip, 15300, true) == ESP_OK);
    assert(registers[14] == 0x83 && registers[15] == 255 && registers[1] == 0x11);
    before = writes;
    assert(rtc_pcf8563_set_countdown_device(&chip, 256, false) == ESP_ERR_INVALID_ARG);
    assert(rtc_pcf8563_set_countdown_device(&chip, 15360, false) == ESP_ERR_INVALID_ARG);
    assert(rtc_pcf8563_set_countdown_device(&chip, 0, false) == ESP_ERR_INVALID_ARG && writes == before);
    uint32_t interrupts;
    registers[1] |= 0x0c;
    assert(rtc_pcf8563_get_interrupt_status_device(&chip, &interrupts) == ESP_OK && interrupts == 3);
    assert(registers[14] == 0x83); /* Repeating countdown keeps running. */
    assert(rtc_pcf8563_clear_interrupt_status_device(&chip, RTC_PCF8563_INTERRUPT_COUNTDOWN) == ESP_OK);
    assert((registers[1] & 0x0c) == 8 && registers[14] == 0x83);
    assert(rtc_pcf8563_set_countdown_device(&chip, 1, false) == ESP_OK);
    registers[1] |= 4;
    assert(rtc_pcf8563_get_interrupt_status_device(&chip, &interrupts) == ESP_OK);
    assert(registers[14] == 3 && (registers[1] & 4)); /* One-shot latches until ack. */
    assert(rtc_pcf8563_clear_interrupt_status_device(&chip, RTC_PCF8563_INTERRUPT_COUNTDOWN) == ESP_OK);
    assert((registers[1] & 4) == 0);
    assert(rtc_pcf8563_disable_countdown_device(&chip) == ESP_OK);
    assert((registers[1] & 0x15) == 0);
    assert(rtc_pcf8563_clear_interrupt_status_device(&chip, 4) == ESP_ERR_INVALID_ARG);

    const solar_os_expansion_binding_t bindings[] = {
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target="i2c0"},
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value=0x51},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="irq", .value=10},
    };
    provider_error = ESP_ERR_INVALID_STATE;
    assert(solar_os_pcf8563_attach("rtc0", bindings, 3) == ESP_ERR_INVALID_STATE);
    provider_error = ESP_OK;
    assert(solar_os_pcf8563_attach("rtc0", bindings, 3) == ESP_OK);
    assert(solar_os_pcf8563_attach("rtc0", bindings, 3) == ESP_ERR_INVALID_STATE);
    assert(provider.interrupt_gpio == 10 && provider.interrupt_active_level == 0);
    solar_os_datetime_t datetime;
    assert(provider.get_utc_datetime(provider.user, &datetime) == ESP_OK && datetime.year == 2026);
    datetime.year = 2028; datetime.month = 2; datetime.day = 29;
    assert(provider.set_utc_datetime(provider.user, &datetime) == ESP_OK);
    const solar_os_rtc_alarm_t unsupported = {.match_fields=SOLAR_OS_RTC_ALARM_MATCH_SECOND |
        SOLAR_OS_RTC_ALARM_MATCH_MINUTE, .second=10, .minute=42};
    assert(provider.set_alarm(provider.user, &unsupported) == ESP_ERR_NOT_SUPPORTED);
    provider_error = ESP_ERR_INVALID_STATE;
    assert(solar_os_pcf8563_detach("rtc0") == ESP_ERR_INVALID_STATE);
    assert(provider.get_utc_datetime(provider.user, &datetime) == ESP_OK);
    provider_error = ESP_OK;
    assert(solar_os_pcf8563_detach("wrong") == ESP_ERR_NOT_FOUND);
    assert(solar_os_pcf8563_detach("rtc0") == ESP_OK);
    assert(solar_os_pcf8563_attach("rtc0", bindings, 2) == ESP_OK);
    assert(provider.interrupt_gpio == -1);
    assert(solar_os_pcf8563_detach("rtc0") == ESP_OK);

    /* A fresh RTC can ACK while its calendar contains undefined bytes. Reads
     * must preserve that state, reject the time, and report only one warning. */
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x08; /* Power-on TESTC bit. */
    registers[2] = 0x80; /* VL set; zero day/month is not a valid date. */
    warning_logs = error_logs = 0;
    assert(solar_os_pcf8563_attach("rtc0", bindings, 2) == ESP_OK);
    uint8_t saved[sizeof(registers)];
    memcpy(saved, registers, sizeof(saved));
    before = writes;
    const solar_os_datetime_t sentinel = {.year=2099, .month=12, .day=31};
    memcpy(&datetime, &sentinel, sizeof(datetime));
    for (unsigned i = 0; i < 100; ++i) {
        assert(provider.get_utc_datetime(provider.user, &datetime) == ESP_ERR_INVALID_RESPONSE);
        assert(memcmp(&datetime, &sentinel, sizeof(datetime)) == 0);
    }
    assert(warning_logs == 1 && error_logs == 0);
    assert(strstr(last_log, "date YYYY-MM-DD") && strstr(last_log, "or ntp"));
    assert(writes == before && memcmp(registers, saved, sizeof(saved)) == 0);

    /* A changed failure still reports transport trouble, without repetition. */
    fail_read = 0;
    for (unsigned i = 0; i < 100; ++i)
        assert(provider.get_utc_datetime(provider.user, &datetime) == ESP_ERR_TIMEOUT);
    assert(warning_logs == 1 && error_logs == 1);
    assert(strstr(last_log, "ESP_ERR_TIMEOUT"));
    fail_read = -1;
    assert(provider.get_utc_datetime(provider.user, &datetime) == ESP_ERR_INVALID_RESPONSE);
    assert(warning_logs == 2 && error_logs == 1);

    /* The normal date/NTP setter recovers without a readable old calendar. */
    datetime = (solar_os_datetime_t){.year=2026, .month=11, .day=2,
        .hour=12, .minute=34, .second=56, .clock_integrity=true};
    fail_write = 2;
    assert(provider.set_utc_datetime(provider.user, &datetime) == ESP_ERR_TIMEOUT);
    assert(provider.get_utc_datetime(provider.user, &datetime) == ESP_ERR_INVALID_RESPONSE);
    assert(warning_logs == 2 && error_logs == 1);
    fail_write = -1;
    assert(provider.set_utc_datetime(provider.user, &datetime) == ESP_OK);
    assert(provider.get_utc_datetime(provider.user, &datetime) == ESP_OK);
    assert(datetime.year == 2026 && datetime.month == 11 && datetime.day == 2 &&
           datetime.hour == 12 && datetime.minute == 34 && datetime.second == 56 &&
           datetime.clock_integrity);
    assert(registers[0] == 0 && (registers[2] & 0x80) == 0);

    registers[3] = 0x1a; /* New corruption after recovery starts a new episode. */
    assert(provider.get_utc_datetime(provider.user, &datetime) == ESP_ERR_INVALID_RESPONSE);
    assert(warning_logs == 3 && error_logs == 1);
    registers[3] = 0x34;
    assert(provider.get_utc_datetime(provider.user, &datetime) == ESP_OK);

    /* Reattaching preserves valid battery-backed time without any writes. */
    assert(solar_os_pcf8563_detach("rtc0") == ESP_OK);
    memcpy(saved, registers, sizeof(saved));
    before = writes;
    assert(solar_os_pcf8563_attach("rtc0", bindings, 2) == ESP_OK);
    assert(provider.get_utc_datetime(provider.user, &datetime) == ESP_OK && datetime.clock_integrity);
    assert(writes == before && memcmp(registers, saved, sizeof(saved)) == 0);
    registers[7] |= 0x80; /* Unsupported century convention is also recoverable. */
    for (unsigned i = 0; i < 100; ++i)
        assert(provider.get_utc_datetime(provider.user, &datetime) == ESP_ERR_NOT_SUPPORTED);
    assert(warning_logs == 4 && error_logs == 1);
    assert(provider.set_utc_datetime(provider.user, &datetime) == ESP_OK);
    assert(provider.get_utc_datetime(provider.user, &datetime) == ESP_OK && datetime.clock_integrity);
    assert(solar_os_pcf8563_detach("rtc0") == ESP_OK);
    puts("PCF8563 / BM8563 driver/provider tests passed");
    return 0;
}
