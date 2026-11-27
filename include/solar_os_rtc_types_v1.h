#pragma once

#include <stdbool.h>
#include <stdint.h>

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

typedef struct {
    char provider[SOLAR_OS_RTC_PROVIDER_NAME_MAX];
    uint32_t capabilities;
    int interrupt_gpio;
    int interrupt_active_level;
    char alarm_owner[SOLAR_OS_RTC_OWNER_NAME_MAX];
    char countdown_owner[SOLAR_OS_RTC_OWNER_NAME_MAX];
} solar_os_rtc_info_t;

typedef enum {
    SOLAR_OS_RTC_TIMER_ALARM,
    SOLAR_OS_RTC_TIMER_COUNTDOWN,
} solar_os_rtc_timer_kind_t;

typedef struct {
    solar_os_rtc_timer_kind_t kind;
    union {
        solar_os_rtc_alarm_t alarm;
        struct {
            uint32_t seconds;
            bool repeat;
        } countdown;
    } configuration;
} solar_os_rtc_timer_request_v1_t;
