#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_rtc.h"

static solar_os_time_provider_t registered_time_provider;
static char registered_time_owner[SOLAR_OS_RTC_PROVIDER_NAME_MAX];
static solar_os_rtc_alarm_t last_alarm;
static uint32_t last_countdown_seconds;
static uint32_t last_cleared_interrupts;
static bool last_countdown_repeat;
static bool alarm_disabled;
static bool countdown_disabled;
static int user_token;

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

esp_err_t solar_os_time_register_provider(
    const char *owner,
    const solar_os_time_provider_t *provider)
{
    if (registered_time_owner[0] != '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(registered_time_owner, owner, sizeof(registered_time_owner));
    registered_time_provider = *provider;
    return ESP_OK;
}

esp_err_t solar_os_time_unregister_provider(const char *owner)
{
    if (strcmp(registered_time_owner, owner) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    registered_time_owner[0] = '\0';
    memset(&registered_time_provider, 0, sizeof(registered_time_provider));
    return ESP_OK;
}

static esp_err_t get_time(void *user, solar_os_datetime_t *datetime)
{
    (void)user;
    (void)datetime;
    return ESP_OK;
}

static esp_err_t set_time(void *user, const solar_os_datetime_t *datetime)
{
    (void)user;
    (void)datetime;
    return ESP_OK;
}

static esp_err_t set_alarm(void *user, const solar_os_rtc_alarm_t *alarm)
{
    assert(user == &user_token);
    last_alarm = *alarm;
    return ESP_OK;
}

static esp_err_t disable_alarm(void *user)
{
    assert(user == &user_token);
    alarm_disabled = true;
    return ESP_OK;
}

static esp_err_t set_countdown(void *user, uint32_t period_seconds, bool repeat)
{
    assert(user == &user_token);
    last_countdown_seconds = period_seconds;
    last_countdown_repeat = repeat;
    return ESP_OK;
}

static esp_err_t disable_countdown(void *user)
{
    assert(user == &user_token);
    countdown_disabled = true;
    return ESP_OK;
}

static esp_err_t get_interrupt_status(void *user, uint32_t *interrupts)
{
    assert(user == &user_token);
    *interrupts = SOLAR_OS_RTC_INTERRUPT_ALARM;
    return ESP_OK;
}

static esp_err_t clear_interrupt_status(void *user, uint32_t interrupts)
{
    assert(user == &user_token);
    last_cleared_interrupts = interrupts;
    return ESP_OK;
}

int main(void)
{
    solar_os_rtc_info_t info;
    assert(!solar_os_rtc_has_provider());
    assert(solar_os_rtc_get_info(&info) == ESP_ERR_NOT_SUPPORTED);

    const solar_os_rtc_provider_t unwired = {
        .get_utc_datetime = get_time,
        .set_utc_datetime = set_time,
        .interrupt_gpio = SOLAR_OS_RTC_INTERRUPT_GPIO_NONE,
    };
    assert(solar_os_rtc_register_provider("external-rtc", &unwired) == ESP_OK);
    assert(solar_os_rtc_has_provider());
    assert(solar_os_rtc_get_info(&info) == ESP_OK);
    assert(strcmp(info.provider, "external-rtc") == 0);
    assert(info.capabilities == SOLAR_OS_RTC_CAP_CALENDAR);
    assert(info.interrupt_gpio == SOLAR_OS_RTC_INTERRUPT_GPIO_NONE);
    assert(solar_os_rtc_set_countdown(1, false) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_rtc_unregister_provider("other") == ESP_ERR_NOT_FOUND);
    assert(solar_os_rtc_unregister_provider("external-rtc") == ESP_OK);

    const solar_os_rtc_provider_t waveshare = {
        .get_utc_datetime = get_time,
        .set_utc_datetime = set_time,
        .set_alarm = set_alarm,
        .disable_alarm = disable_alarm,
        .set_countdown = set_countdown,
        .disable_countdown = disable_countdown,
        .get_interrupt_status = get_interrupt_status,
        .clear_interrupt_status = clear_interrupt_status,
        .user = &user_token,
        .interrupt_gpio = 15,
    };
    assert(solar_os_rtc_register_provider("rtc0", &waveshare) == ESP_OK);
    assert(solar_os_rtc_get_info(&info) == ESP_OK);
    assert(strcmp(info.provider, "rtc0") == 0);
    assert(info.capabilities == (SOLAR_OS_RTC_CAP_CALENDAR |
                                 SOLAR_OS_RTC_CAP_ALARM |
                                 SOLAR_OS_RTC_CAP_COUNTDOWN |
                                 SOLAR_OS_RTC_CAP_INTERRUPT_STATUS));
    assert(info.interrupt_gpio == 15);
    assert(registered_time_provider.get_utc_datetime == get_time);
    assert(registered_time_provider.set_utc_datetime == set_time);

    const solar_os_rtc_alarm_t alarm = {
        .match_fields = SOLAR_OS_RTC_ALARM_MATCH_HOUR |
                        SOLAR_OS_RTC_ALARM_MATCH_MINUTE,
        .hour = 7,
        .minute = 30,
    };
    assert(solar_os_rtc_set_alarm(&alarm) == ESP_OK);
    assert(last_alarm.match_fields == alarm.match_fields);
    assert(last_alarm.hour == 7);
    assert(last_alarm.minute == 30);
    assert(solar_os_rtc_disable_alarm() == ESP_OK);
    assert(alarm_disabled);
    assert(solar_os_rtc_set_countdown(90, true) == ESP_OK);
    assert(last_countdown_seconds == 90);
    assert(last_countdown_repeat);
    assert(solar_os_rtc_disable_countdown() == ESP_OK);
    assert(countdown_disabled);

    uint32_t interrupts = 0;
    assert(solar_os_rtc_get_interrupt_status(&interrupts) == ESP_OK);
    assert(interrupts == SOLAR_OS_RTC_INTERRUPT_ALARM);
    assert(solar_os_rtc_clear_interrupt_status(
        SOLAR_OS_RTC_INTERRUPT_ALARM | SOLAR_OS_RTC_INTERRUPT_COUNTDOWN) == ESP_OK);
    assert(last_cleared_interrupts == (SOLAR_OS_RTC_INTERRUPT_ALARM |
                                       SOLAR_OS_RTC_INTERRUPT_COUNTDOWN));
    assert(solar_os_rtc_unregister_provider("rtc0") == ESP_OK);
    assert(!solar_os_rtc_has_provider());

    puts("rtc service tests passed");
    return 0;
}
