#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define RTC_RX8130_I2C_ADDR_DEFAULT 0x32U

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t weekday;
    bool clock_integrity;
} rtc_rx8130_datetime_t;

typedef struct {
    uint32_t match_fields;
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t weekday;
} rtc_rx8130_alarm_t;

#define RTC_RX8130_ALARM_MATCH_MINUTE (1U << 0)
#define RTC_RX8130_ALARM_MATCH_HOUR   (1U << 1)
#define RTC_RX8130_ALARM_MATCH_DAY    (1U << 2)
#define RTC_RX8130_ALARM_MATCH_WEEKDAY (1U << 3)

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    void *ctx;
} rtc_rx8130_io_t;

typedef struct {
    rtc_rx8130_io_t io;
    bool initialized;
} rtc_rx8130_t;

esp_err_t rtc_rx8130_init(rtc_rx8130_t *device, const rtc_rx8130_io_t *io);
esp_err_t rtc_rx8130_deinit(rtc_rx8130_t *device);
esp_err_t rtc_rx8130_get_datetime(rtc_rx8130_t *device, rtc_rx8130_datetime_t *datetime);
esp_err_t rtc_rx8130_set_datetime(rtc_rx8130_t *device, const rtc_rx8130_datetime_t *datetime);
esp_err_t rtc_rx8130_set_alarm(rtc_rx8130_t *device, const rtc_rx8130_alarm_t *alarm);
esp_err_t rtc_rx8130_disable_alarm(rtc_rx8130_t *device);
esp_err_t rtc_rx8130_set_countdown(rtc_rx8130_t *device, uint32_t period_seconds, bool repeat);
esp_err_t rtc_rx8130_disable_countdown(rtc_rx8130_t *device);
