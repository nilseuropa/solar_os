#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_time.h"

#define SOLAR_OS_RTC_PROVIDER_NAME_MAX 24
#define SOLAR_OS_RTC_OWNER_NAME_MAX 24
#define SOLAR_OS_RTC_INTERRUPT_GPIO_NONE (-1)

typedef enum {
    SOLAR_OS_RTC_CAP_CALENDAR = 1U << 0,
    SOLAR_OS_RTC_CAP_ALARM = 1U << 1,
    SOLAR_OS_RTC_CAP_COUNTDOWN = 1U << 2,
    SOLAR_OS_RTC_CAP_INTERRUPT_STATUS = 1U << 3,
} solar_os_rtc_capability_t;

typedef enum {
    SOLAR_OS_RTC_ALARM_MATCH_SECOND = 1U << 0,
    SOLAR_OS_RTC_ALARM_MATCH_MINUTE = 1U << 1,
    SOLAR_OS_RTC_ALARM_MATCH_HOUR = 1U << 2,
    SOLAR_OS_RTC_ALARM_MATCH_DAY = 1U << 3,
    SOLAR_OS_RTC_ALARM_MATCH_WEEKDAY = 1U << 4,
} solar_os_rtc_alarm_match_t;

typedef struct {
    uint32_t match_fields;
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t weekday;
} solar_os_rtc_alarm_t;

typedef enum {
    SOLAR_OS_RTC_INTERRUPT_ALARM = 1U << 0,
    SOLAR_OS_RTC_INTERRUPT_COUNTDOWN = 1U << 1,
} solar_os_rtc_interrupt_t;

typedef esp_err_t (*solar_os_rtc_set_alarm_fn_t)(
    void *user,
    const solar_os_rtc_alarm_t *alarm);
typedef esp_err_t (*solar_os_rtc_disable_alarm_fn_t)(void *user);
typedef esp_err_t (*solar_os_rtc_set_countdown_fn_t)(
    void *user,
    uint32_t period_seconds,
    bool repeat);
typedef esp_err_t (*solar_os_rtc_disable_countdown_fn_t)(void *user);
typedef esp_err_t (*solar_os_rtc_get_interrupt_status_fn_t)(
    void *user,
    uint32_t *interrupts);
typedef esp_err_t (*solar_os_rtc_clear_interrupt_status_fn_t)(
    void *user,
    uint32_t interrupts);

typedef struct {
    solar_os_time_provider_get_fn_t get_utc_datetime;
    solar_os_time_provider_set_fn_t set_utc_datetime;
    solar_os_rtc_set_alarm_fn_t set_alarm;
    solar_os_rtc_disable_alarm_fn_t disable_alarm;
    solar_os_rtc_set_countdown_fn_t set_countdown;
    solar_os_rtc_disable_countdown_fn_t disable_countdown;
    solar_os_rtc_get_interrupt_status_fn_t get_interrupt_status;
    solar_os_rtc_clear_interrupt_status_fn_t clear_interrupt_status;
    void *user;
    int interrupt_gpio;
    int interrupt_active_level;
} solar_os_rtc_provider_t;

typedef struct {
    char provider[SOLAR_OS_RTC_PROVIDER_NAME_MAX];
    uint32_t capabilities;
    int interrupt_gpio;
    int interrupt_active_level;
    char alarm_owner[SOLAR_OS_RTC_OWNER_NAME_MAX];
    char countdown_owner[SOLAR_OS_RTC_OWNER_NAME_MAX];
} solar_os_rtc_info_t;

esp_err_t solar_os_rtc_register_provider(
    const char *owner,
    const solar_os_rtc_provider_t *provider);
esp_err_t solar_os_rtc_unregister_provider(const char *owner);
bool solar_os_rtc_has_provider(void);
esp_err_t solar_os_rtc_get_info(solar_os_rtc_info_t *info);
esp_err_t solar_os_rtc_set_alarm_for(const char *owner,
                                     const solar_os_rtc_alarm_t *alarm);
esp_err_t solar_os_rtc_disable_alarm_for(const char *owner);
esp_err_t solar_os_rtc_set_countdown_for(const char *owner,
                                         uint32_t period_seconds,
                                         bool repeat);
esp_err_t solar_os_rtc_disable_countdown_for(const char *owner);
void solar_os_rtc_release_owner(const char *owner);
esp_err_t solar_os_rtc_set_alarm(const solar_os_rtc_alarm_t *alarm);
esp_err_t solar_os_rtc_disable_alarm(void);
esp_err_t solar_os_rtc_set_countdown(uint32_t period_seconds, bool repeat);
esp_err_t solar_os_rtc_disable_countdown(void);
esp_err_t solar_os_rtc_get_interrupt_status(uint32_t *interrupts);
esp_err_t solar_os_rtc_clear_interrupt_status(uint32_t interrupts);
