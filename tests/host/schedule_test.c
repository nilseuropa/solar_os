#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvs.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_rtc.h"
#include "solar_os_schedule.h"
#include "solar_os_task.h"
#include "solar_os_time.h"

static uint64_t fake_uptime_ms;
static bool fake_time_valid;
static uint64_t fake_epoch_ms;
static bool fake_rtc_available;
static solar_os_rtc_alarm_t fake_rtc_alarm;
static unsigned rtc_alarm_sets;
static uint32_t fake_rtc_countdown_seconds;
static unsigned nvs_writes;

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "error";
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size > 0) {
        const size_t copy = len < size - 1U ? len : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

esp_err_t solar_os_log_write(solar_os_log_level_t level,
                             const char *tag,
                             const char *fmt,
                             ...)
{
    (void)level;
    (void)tag;
    (void)fmt;
    return ESP_OK;
}

void *solar_os_memory_calloc(size_t count, size_t size,
                             solar_os_memory_class_t memory_class,
                             const char *tag)
{
    (void)memory_class;
    (void)tag;
    return calloc(count, size);
}

void solar_os_memory_free(void *ptr)
{
    free(ptr);
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    (void)name;
    (void)mode;
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                       void *value, size_t *length)
{
    (void)handle;
    (void)key;
    (void)value;
    (void)length;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length)
{
    (void)handle;
    (void)key;
    (void)value;
    (void)length;
    nvs_writes++;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; }

uint64_t solar_os_time_uptime_ms(void) { return fake_uptime_ms; }

esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *epoch_ms)
{
    if (!fake_time_valid) return ESP_ERR_INVALID_STATE;
    *epoch_ms = fake_epoch_ms;
    return ESP_OK;
}

bool solar_os_time_datetime_is_valid(const solar_os_datetime_t *value)
{
    return value != NULL && value->year >= 1970 && value->month >= 1 &&
        value->month <= 12 && value->day >= 1 && value->day <= 31 &&
        value->hour <= 23 && value->minute <= 59 && value->second <= 59;
}

static void tm_to_datetime(const struct tm *tm, solar_os_datetime_t *value)
{
    *value = (solar_os_datetime_t) {
        .year = (uint16_t)(tm->tm_year + 1900),
        .month = (uint8_t)(tm->tm_mon + 1),
        .day = (uint8_t)tm->tm_mday,
        .hour = (uint8_t)tm->tm_hour,
        .minute = (uint8_t)tm->tm_min,
        .second = (uint8_t)tm->tm_sec,
        .weekday = (uint8_t)tm->tm_wday,
        .clock_integrity = true,
    };
}

esp_err_t solar_os_time_get_datetime(solar_os_datetime_t *value)
{
    if (!fake_time_valid) return ESP_ERR_INVALID_STATE;
    time_t seconds = (time_t)(fake_epoch_ms / 1000ULL);
    struct tm tm;
    gmtime_r(&seconds, &tm);
    tm_to_datetime(&tm, value);
    return ESP_OK;
}

esp_err_t solar_os_time_local_to_utc(const solar_os_datetime_t *local,
                                     solar_os_datetime_t *utc)
{
    if (!solar_os_time_datetime_is_valid(local)) return ESP_ERR_INVALID_ARG;
    struct tm tm = {
        .tm_year = local->year - 1900,
        .tm_mon = local->month - 1,
        .tm_mday = local->day,
        .tm_hour = local->hour,
        .tm_min = local->minute,
        .tm_sec = local->second,
    };
    const time_t seconds = timegm(&tm);
    struct tm converted;
    gmtime_r(&seconds, &converted);
    tm_to_datetime(&converted, utc);
    return ESP_OK;
}

esp_err_t solar_os_rtc_get_info(solar_os_rtc_info_t *info)
{
    if (!fake_rtc_available) return ESP_ERR_NOT_SUPPORTED;
    memset(info, 0, sizeof(*info));
    info->capabilities = SOLAR_OS_RTC_CAP_ALARM |
        SOLAR_OS_RTC_CAP_COUNTDOWN |
        SOLAR_OS_RTC_CAP_INTERRUPT_STATUS;
    if (rtc_alarm_sets != 0) strlcpy(info->alarm_owner, "schedule", sizeof(info->alarm_owner));
    if (fake_rtc_countdown_seconds != 0) {
        strlcpy(info->countdown_owner, "schedule", sizeof(info->countdown_owner));
    }
    return ESP_OK;
}
bool solar_os_rtc_has_provider(void) { return fake_rtc_available; }
esp_err_t solar_os_rtc_set_alarm_for(const char *owner, const solar_os_rtc_alarm_t *alarm)
{
    assert(strcmp(owner, "schedule") == 0);
    fake_rtc_alarm = *alarm;
    rtc_alarm_sets++;
    return ESP_OK;
}
esp_err_t solar_os_rtc_disable_alarm_for(const char *owner)
{ assert(strcmp(owner, "schedule") == 0); rtc_alarm_sets = 0; return ESP_OK; }
esp_err_t solar_os_rtc_set_countdown_for(const char *owner,
                                         uint32_t period_seconds,
                                         bool repeat)
{
    assert(strcmp(owner, "schedule") == 0);
    assert(!repeat);
    fake_rtc_countdown_seconds = period_seconds;
    return ESP_OK;
}
esp_err_t solar_os_rtc_disable_countdown_for(const char *owner)
{
    assert(strcmp(owner, "schedule") == 0);
    fake_rtc_countdown_seconds = 0;
    return ESP_OK;
}
esp_err_t solar_os_rtc_get_interrupt_status(uint32_t *interrupts)
{ *interrupts = 0; return fake_rtc_available ? ESP_OK : ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_rtc_clear_interrupt_status(uint32_t interrupts)
{ (void)interrupts; return ESP_ERR_NOT_SUPPORTED; }

BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t task,
                                                const char *name,
                                                uint32_t stack_depth,
                                                void *parameters,
                                                UBaseType_t priority,
                                                TaskHandle_t *handle,
                                                BaseType_t core_id,
                                                solar_os_task_role_t role)
{
    (void)task; (void)name; (void)stack_depth; (void)parameters;
    (void)priority; (void)core_id; (void)role;
    *handle = (TaskHandle_t)1;
    return pdPASS;
}
void solar_os_task_delete_internal(TaskHandle_t task) { (void)task; }

static uint64_t utc_ms(int year, int month, int day, int hour, int minute, int second)
{
    struct tm tm = {
        .tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
        .tm_hour = hour, .tm_min = minute, .tm_sec = second,
    };
    return (uint64_t)timegm(&tm) * 1000ULL;
}

int main(void)
{
    assert(solar_os_schedule_init() == ESP_OK);
    assert(solar_os_schedule_count() == 0);

    assert(solar_os_schedule_add_relative("tea", 2,
        SOLAR_OS_SCHEDULE_ACTION_ALARM, NULL, false) == ESP_OK);
    uint64_t wake_us = 0;
    assert(solar_os_schedule_next_wake_us(&wake_us));
    assert(wake_us == 2000000ULL);
    uint32_t remaining = 0;
    assert(solar_os_schedule_remaining_seconds("tea", &remaining) == ESP_OK);
    assert(remaining == 2);
    fake_uptime_ms = 1999;
    solar_os_schedule_poll();
    assert(!solar_os_schedule_alarm_active(NULL, 0));
    fake_uptime_ms = 2000;
    solar_os_schedule_poll();
    char alarm_name[SOLAR_OS_SCHEDULE_NAME_MAX];
    assert(solar_os_schedule_alarm_active(alarm_name, sizeof(alarm_name)));
    assert(strcmp(alarm_name, "tea") == 0);
    solar_os_schedule_stop_alarm();

    assert(solar_os_schedule_add_interval("hourly", 3600,
        SOLAR_OS_SCHEDULE_ACTION_ALARM, NULL) == ESP_OK);
    assert(nvs_writes == 1);
    assert(solar_os_schedule_set_enabled("hourly", false) == ESP_OK);

    fake_time_valid = true;
    fake_rtc_available = true;
    fake_epoch_ms = utc_ms(2026, 9, 2, 6, 0, 0);
    assert(solar_os_schedule_add_daily("wake", 7, 0, 0,
        SOLAR_OS_SCHEDULE_ACTION_ALARM, NULL) == ESP_OK);
    solar_os_schedule_poll();
    solar_os_schedule_entry_t wake;
    assert(solar_os_schedule_get_by_name("wake", &wake) == ESP_OK);
    assert(wake.next_utc_seconds == utc_ms(2026, 9, 2, 7, 0, 0) / 1000ULL);
    assert(rtc_alarm_sets == 1);
    assert(fake_rtc_alarm.hour == 7 && fake_rtc_alarm.day == 2);

    fake_epoch_ms = utc_ms(2026, 9, 2, 7, 0, 0);
    solar_os_schedule_poll();
    assert(solar_os_schedule_alarm_active(alarm_name, sizeof(alarm_name)));
    assert(strcmp(alarm_name, "wake") == 0);
    assert(solar_os_schedule_get_by_name("wake", &wake) == ESP_OK);
    assert(wake.next_utc_seconds == utc_ms(2026, 9, 3, 7, 0, 0) / 1000ULL);
    assert(rtc_alarm_sets == 2);
    assert(fake_rtc_alarm.hour == 7 && fake_rtc_alarm.day == 3);

    solar_os_schedule_stop_alarm();
    assert(solar_os_schedule_set_enabled("wake", false) == ESP_OK);
    assert(solar_os_schedule_add_at("far", 2026, 11, 2, 7, 0, 0,
        SOLAR_OS_SCHEDULE_ACTION_ALARM, NULL) == ESP_OK);
    solar_os_schedule_poll();
    assert(rtc_alarm_sets == 0);

    fake_time_valid = false;
    assert(solar_os_schedule_add_relative("nap", 90,
        SOLAR_OS_SCHEDULE_ACTION_ALARM, NULL, false) == ESP_OK);
    solar_os_schedule_poll();
    assert(fake_rtc_countdown_seconds == 90);

    puts("schedule tests passed");
    return 0;
}
