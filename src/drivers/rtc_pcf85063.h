#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define RTC_PCF85063_ADDRESS 0x51
#define RTC_PCF85063_BUS_NAME_MAX 16

typedef struct {
    char bus[RTC_PCF85063_BUS_NAME_MAX];
    uint8_t address;
} rtc_pcf85063_t;

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    bool clock_integrity;
} rtc_datetime_t;

typedef enum {
    RTC_PCF85063_ALARM_MATCH_SECOND = 1U << 0,
    RTC_PCF85063_ALARM_MATCH_MINUTE = 1U << 1,
    RTC_PCF85063_ALARM_MATCH_HOUR = 1U << 2,
    RTC_PCF85063_ALARM_MATCH_DAY = 1U << 3,
    RTC_PCF85063_ALARM_MATCH_WEEKDAY = 1U << 4,
} rtc_pcf85063_alarm_match_t;

typedef struct {
    uint32_t match_fields;
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t weekday;
} rtc_pcf85063_alarm_t;

typedef enum {
    RTC_PCF85063_INTERRUPT_ALARM = 1U << 0,
    RTC_PCF85063_INTERRUPT_COUNTDOWN = 1U << 1,
} rtc_pcf85063_interrupt_t;

esp_err_t rtc_pcf85063_init(void);
esp_err_t rtc_pcf85063_get_datetime(rtc_datetime_t *datetime);
esp_err_t rtc_pcf85063_set_datetime(const rtc_datetime_t *datetime);
esp_err_t rtc_pcf85063_init_device(rtc_pcf85063_t *device,
                                   const char *bus,
                                   uint8_t address);
esp_err_t rtc_pcf85063_get_datetime_device(const rtc_pcf85063_t *device,
                                           rtc_datetime_t *datetime);
esp_err_t rtc_pcf85063_set_datetime_device(const rtc_pcf85063_t *device,
                                           const rtc_datetime_t *datetime);
esp_err_t rtc_pcf85063_set_alarm_device(const rtc_pcf85063_t *device,
                                        const rtc_pcf85063_alarm_t *alarm);
esp_err_t rtc_pcf85063_disable_alarm_device(const rtc_pcf85063_t *device);
esp_err_t rtc_pcf85063_set_countdown_device(const rtc_pcf85063_t *device,
                                            uint32_t period_seconds,
                                            bool repeat);
esp_err_t rtc_pcf85063_disable_countdown_device(const rtc_pcf85063_t *device);
esp_err_t rtc_pcf85063_get_interrupt_status_device(const rtc_pcf85063_t *device,
                                                   uint32_t *interrupts);
esp_err_t rtc_pcf85063_clear_interrupt_status_device(const rtc_pcf85063_t *device,
                                                     uint32_t interrupts);
bool rtc_pcf85063_datetime_is_valid(const rtc_datetime_t *datetime);
