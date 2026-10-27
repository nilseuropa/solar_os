#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtc_ds3231.h"
#include "solar_os_ds3231.h"
#include "solar_os_rtc.h"

extern const solar_os_expansion_driver_t solar_os_ds3231_expansion_driver;

static uint8_t registers[19];
static int fail_read = -1, fail_write = -1;
static unsigned writes;
static uint8_t status_race;
static bool partial_calendar_write;
static solar_os_time_provider_t time_provider;
static char time_owner[SOLAR_OS_RTC_PROVIDER_NAME_MAX];
static esp_err_t time_error;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n); dst[n] = 0;
    }
    return len;
}

esp_err_t solar_os_bus_i2c_read_reg(const char *bus, uint8_t addr,
    uint8_t reg, uint8_t *data, size_t len)
{
    assert(strcmp(bus, "i2c0") == 0 && addr == 0x68);
    assert((size_t)reg + len <= sizeof(registers));
    if (reg == fail_read) return ESP_ERR_TIMEOUT;
    memcpy(data, registers + reg, len);
    if (reg == 15) { registers[15] |= status_race; status_race = 0; }
    return ESP_OK;
}

esp_err_t solar_os_bus_i2c_write_reg(const char *bus, uint8_t addr,
    uint8_t reg, const uint8_t *data, size_t len)
{
    assert(strcmp(bus, "i2c0") == 0 && addr == 0x68);
    assert((size_t)reg + len <= sizeof(registers));
    if (reg == fail_write) {
        if (reg == 0 && partial_calendar_write) registers[0] = data[0];
        return ESP_ERR_TIMEOUT;
    }
    ++writes;
    for (size_t i = 0; i < len; ++i) {
        if (reg + i == 15) {
            /* OSF/A1F/A2F are W0C; BSY is read-only; EN32kHz is writable. */
            registers[15] = (uint8_t)((data[i] & 8U) | (registers[15] & 4U) |
                (registers[15] & data[i] & 0x83U));
        } else registers[reg + i] = data[i];
    }
    return ESP_OK;
}

esp_err_t solar_os_time_register_provider(const char *name, const solar_os_time_provider_t *value)
{
    if (time_error != ESP_OK) return time_error;
    if (time_owner[0]) return ESP_ERR_INVALID_STATE;
    strlcpy(time_owner, name, sizeof(time_owner));
    time_provider = *value;
    return ESP_OK;
}

esp_err_t solar_os_time_unregister_provider(const char *name)
{
    if (time_error != ESP_OK) return time_error;
    if (strcmp(time_owner, name)) return ESP_ERR_NOT_FOUND;
    time_owner[0] = 0;
    memset(&time_provider, 0, sizeof(time_provider));
    return ESP_OK;
}

static void test_calendar(void)
{
    solar_os_ds3231_t chip = {0};
    solar_os_ds3231_datetime_t value;
    const uint8_t calendar[] = {0x05, 0x42, 0x07, 0x05, 0x29, 0x02, 0x24};
    memcpy(registers, calendar, sizeof(calendar));
    registers[14] = 0xdb; registers[15] = 0x8b; registers[16] = 0xf1;
    unsigned before = writes;
    assert(solar_os_ds3231_init_device(&chip, "i2c0", 0x67) == ESP_ERR_INVALID_ARG);
    assert(solar_os_ds3231_init_device(&chip, "0123456789012345", 0x68) == ESP_ERR_INVALID_ARG);
    fail_read = 14;
    assert(solar_os_ds3231_init_device(&chip, "i2c0", 0x68) == ESP_ERR_TIMEOUT);
    assert(chip.bus[0] == 0 && writes == before);
    fail_read = -1; fail_write = 14;
    assert(solar_os_ds3231_init_device(&chip, "i2c0", 0x68) == ESP_ERR_TIMEOUT);
    fail_write = -1;
    assert(solar_os_ds3231_init_device(&chip, "i2c0", 0x68) == ESP_OK);
    assert(writes == before + 1 && registers[14] == 0x5b && registers[15] == 0x8b);
    assert(!memcmp(registers, calendar, sizeof(calendar)) && registers[16] == 0xf1);
    assert(solar_os_ds3231_get_datetime_device(&chip, &value) == ESP_OK);
    assert(value.year == 2024 && value.month == 2 && value.day == 29 && value.weekday == 4 &&
           value.hour == 7 && value.minute == 42 && value.second == 5 && !value.clock_integrity);
    registers[15] = 8; /* Init's EOSC observation stays unreliable until time is set. */
    assert(solar_os_ds3231_get_datetime_device(&chip, &value) == ESP_OK && !value.clock_integrity);
    assert(solar_os_ds3231_set_datetime_device(&chip, &value) == ESP_OK);
    assert(solar_os_ds3231_get_datetime_device(&chip, &value) == ESP_OK && value.clock_integrity);
    const uint8_t hours[] = {0x52, 0x72, 0x61, 0x23};
    const uint8_t decoded[] = {0, 12, 13, 23};
    for (size_t i = 0; i < sizeof(hours); ++i) {
        registers[2] = hours[i];
        assert(solar_os_ds3231_get_datetime_device(&chip, &value) == ESP_OK && value.hour == decoded[i]);
    }
    const uint8_t invalid_hours[] = {0x40, 0x53, 0x24, 0x1a, 0x87};
    for (size_t i = 0; i < sizeof(invalid_hours); ++i) {
        registers[2] = invalid_hours[i];
        assert(solar_os_ds3231_get_datetime_device(&chip, &value) == ESP_ERR_INVALID_RESPONSE);
    }
    registers[2] = 7; registers[6] = 0x23;
    assert(solar_os_ds3231_get_datetime_device(&chip, &value) == ESP_ERR_INVALID_RESPONSE);
    registers[6] = 0x24; registers[1] = 0x1a;
    assert(solar_os_ds3231_get_datetime_device(&chip, &value) == ESP_ERR_INVALID_RESPONSE);
    registers[1] = 0x42; registers[5] = 0x82;
    assert(solar_os_ds3231_get_datetime_device(&chip, &value) == ESP_ERR_NOT_SUPPORTED);
    registers[5] = 2; registers[3] = 0;
    assert(solar_os_ds3231_get_datetime_device(&chip, &value) == ESP_ERR_INVALID_RESPONSE);
    registers[3] = 5;
    value = (solar_os_ds3231_datetime_t){.year=2026, .month=10, .day=14, .hour=12, .minute=34, .second=56};
    registers[15] = 0x8f; status_race = 3;
    assert(solar_os_ds3231_set_datetime_device(&chip, &value) == ESP_OK);
    const uint8_t expected[] = {0x56, 0x34, 0x12, 4, 0x14, 0x10, 0x26};
    assert(!memcmp(registers, expected, sizeof(expected)));
    assert(registers[15] == 15 && registers[16] == 0xf1); /* Alarm flags/output/BSY survive. */
    value.year = 2100;
    before = writes;
    assert(solar_os_ds3231_set_datetime_device(&chip, &value) == ESP_ERR_INVALID_ARG && writes == before);
    value.year = 2026; value.month = 2; value.day = 29;
    assert(solar_os_ds3231_set_datetime_device(&chip, &value) == ESP_ERR_INVALID_ARG);
    value.month = 10; value.day = 14; value.second = 1;
    fail_write = 0; partial_calendar_write = true;
    assert(solar_os_ds3231_set_datetime_device(&chip, &value) == ESP_ERR_TIMEOUT);
    assert(registers[0] == 1);
    assert(solar_os_ds3231_get_datetime_device(&chip, &value) == ESP_OK && !value.clock_integrity);
    fail_write = 15;
    assert(solar_os_ds3231_set_datetime_device(&chip, &value) == ESP_ERR_TIMEOUT);
    assert(chip.clock_integrity_lost);
    fail_write = -1;
    assert(solar_os_ds3231_set_datetime_device(&chip, &value) == ESP_OK);
    registers[15] |= 0x80;
    assert(solar_os_ds3231_get_datetime_device(&chip, &value) == ESP_OK && !value.clock_integrity);
    fail_read = 0;
    assert(solar_os_ds3231_get_datetime_device(&chip, &value) == ESP_ERR_TIMEOUT);
    fail_read = -1;
}

static void test_alarms(void)
{
    solar_os_ds3231_t chip;
    assert(solar_os_ds3231_init_device(&chip, "i2c0", 0x68) == ESP_OK);
    solar_os_ds3231_alarm_t alarm = {.second=10, .minute=42, .hour=7, .day=14, .weekday=0};
    /* Exhaust all match masks: only documented patterns may write hardware. */
    for (unsigned fields = 0; fields < 64; ++fields) {
        alarm.match_fields = fields;
        unsigned before = writes;
        const bool supported = fields == 1 || fields == 3 || fields == 7 || fields == 15 || fields == 23;
        const esp_err_t expected = supported ? ESP_OK : (!fields || fields >= 32 ?
            ESP_ERR_INVALID_ARG : ESP_ERR_NOT_SUPPORTED);
        registers[14] = 0x5b; registers[15] = 0x8f;
        assert(solar_os_ds3231_set_alarm_device(&chip, &alarm) == expected);
        if (!supported) { assert(writes == before); continue; }
        assert(registers[7] == 0x10 && registers[8] == ((fields & 2) ? 0x42 : 0x80));
        assert(registers[9] == ((fields & 4) ? 7 : 0x80));
        assert(registers[10] == ((fields & 8) ? 0x14 : ((fields & 16) ? 0x41 : 0x80)));
        assert(registers[14] == 0x5d && registers[15] == 0x8c);
    }
    alarm.match_fields = 7; alarm.second = 60;
    unsigned before = writes;
    assert(solar_os_ds3231_set_alarm_device(&chip, &alarm) == ESP_ERR_INVALID_ARG && writes == before);
    alarm.second = 10;
    fail_write = 7;
    assert(solar_os_ds3231_set_alarm_device(&chip, &alarm) == ESP_ERR_TIMEOUT);
    assert(!(registers[14] & 3)); /* Failed configuration leaves both interrupts disabled. */
    fail_write = 15;
    assert(solar_os_ds3231_set_alarm_device(&chip, &alarm) == ESP_ERR_TIMEOUT);
    assert(!(registers[14] & 3));
    fail_write = -1;
    registers[15] = 9; status_race = 0x82;
    assert(solar_os_ds3231_clear_interrupt_status_device(&chip, 1) == ESP_OK);
    assert(registers[15] == 0x88); /* OSF arriving during alarm ack survives. */
    registers[15] = 0x8b;
    uint32_t interrupts;
    assert(solar_os_ds3231_get_interrupt_status_device(&chip, &interrupts) == ESP_OK && interrupts == 1);
    assert(solar_os_ds3231_clear_interrupt_status_device(&chip, 2) == ESP_ERR_INVALID_ARG);
    assert(solar_os_ds3231_disable_alarm_device(&chip) == ESP_OK);
    assert(!(registers[14] & 3) && registers[15] == 0x88);
}

static void test_expansion(void)
{
    const solar_os_expansion_driver_t *driver = &solar_os_ds3231_expansion_driver;
    assert(!strcmp(driver->name, "ds3231") && driver->probe_supported && driver->binding_spec_count == 3);
    assert(driver->binding_specs[1].allowed_value_count == 1 && driver->binding_specs[1].allowed_values[0] == 0x68);
    solar_os_expansion_binding_t bindings[] = {
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target="i2c0"},
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value=0x68},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="irq", .value=10},
    };
    assert(driver->attach("rtc0", bindings, 1) == ESP_ERR_INVALID_ARG);
    bindings[1].value = 0x69;
    assert(driver->attach("rtc0", bindings, 3) == ESP_ERR_INVALID_ARG);
    bindings[1].value = 0x68;
    bindings[2].value = 64;
    assert(driver->attach("rtc0", bindings, 3) == ESP_ERR_INVALID_ARG);
    bindings[2].value = 10;
    time_error = ESP_ERR_INVALID_STATE;
    assert(driver->attach("rtc0", bindings, 3) == ESP_ERR_INVALID_STATE);
    time_error = ESP_OK; fail_write = 14;
    assert(driver->attach("rtc0", bindings, 3) == ESP_ERR_TIMEOUT);
    assert(!solar_os_rtc_has_provider() && !time_owner[0]); /* Actual provider registration rolls back. */
    fail_write = -1; registers[14] |= 3; registers[15] |= 3;
    assert(driver->attach("rtc0", bindings, 3) == ESP_OK);
    assert(!(registers[14] & 3) && !(registers[15] & 3));
    assert(driver->attach("rtc1", bindings, 3) == ESP_ERR_INVALID_STATE);
    solar_os_rtc_info_t info;
    assert(solar_os_rtc_get_info(&info) == ESP_OK);
    assert(!strcmp(info.provider, "rtc0") && info.interrupt_gpio == 10 && info.interrupt_active_level == 0);
    assert(info.capabilities == (SOLAR_OS_RTC_CAP_CALENDAR | SOLAR_OS_RTC_CAP_ALARM | SOLAR_OS_RTC_CAP_INTERRUPT_STATUS));
    solar_os_datetime_t datetime;
    assert(time_provider.get_utc_datetime(time_provider.user, &datetime) == ESP_OK && datetime.year == 2026);
    datetime.year = 2028; datetime.month = 2; datetime.day = 29;
    assert(time_provider.set_utc_datetime(time_provider.user, &datetime) == ESP_OK);
    assert(time_provider.get_utc_datetime(time_provider.user, &datetime) == ESP_OK && datetime.clock_integrity);
    const solar_os_rtc_alarm_t alarm = {.match_fields=15, .second=17, .minute=42, .hour=7, .day=14};
    assert(solar_os_rtc_set_alarm_for("schedule", &alarm) == ESP_OK && registers[7] == 0x17);
    assert(solar_os_rtc_set_alarm_for("shell", &alarm) == ESP_ERR_INVALID_STATE);
    assert(solar_os_rtc_set_countdown(10, false) == ESP_ERR_NOT_SUPPORTED);
    registers[15] |= 1;
    uint32_t interrupts;
    assert(solar_os_rtc_get_interrupt_status(&interrupts) == ESP_OK && interrupts == SOLAR_OS_RTC_INTERRUPT_ALARM);
    assert(solar_os_rtc_clear_interrupt_status(SOLAR_OS_RTC_INTERRUPT_ALARM) == ESP_OK);
    assert(!(registers[15] & 1));
    assert(driver->detach("wrong") == ESP_ERR_NOT_FOUND);
    fail_write = 14;
    assert(driver->detach("rtc0") == ESP_ERR_TIMEOUT && solar_os_rtc_has_provider());
    fail_write = -1; time_error = ESP_ERR_INVALID_STATE;
    assert(driver->detach("rtc0") == ESP_ERR_INVALID_STATE);
    assert(time_provider.get_utc_datetime(time_provider.user, &datetime) == ESP_OK);
    time_error = ESP_OK;
    const solar_os_time_provider_t saved_time_provider = time_provider;
    assert(driver->detach("rtc0") == ESP_OK && !solar_os_rtc_has_provider());
    assert(!(registers[14] & 3));
    assert(driver->attach("rtc0", bindings, 2) == ESP_OK);
    assert(solar_os_rtc_get_info(&info) == ESP_OK && info.interrupt_gpio == -1 && !info.alarm_owner[0]);
    assert(driver->detach("rtc0") == ESP_OK);
    /* A different active RTC must not be disturbed by DS3231 attachment. */
    const solar_os_rtc_provider_t other = {
        .get_utc_datetime = saved_time_provider.get_utc_datetime,
        .set_utc_datetime = saved_time_provider.set_utc_datetime,
        .interrupt_gpio = SOLAR_OS_RTC_INTERRUPT_GPIO_NONE,
    };
    assert(solar_os_rtc_register_provider("onboard", &other) == ESP_OK);
    unsigned before = writes;
    assert(driver->attach("rtc0", bindings, 3) == ESP_ERR_INVALID_STATE && writes == before);
    assert(solar_os_rtc_unregister_provider("onboard") == ESP_OK);
}

int main(void)
{
    test_calendar();
    test_alarms();
    test_expansion();
    puts("DS3231 calendar, alarm and expansion/provider tests passed");
    return 0;
}
