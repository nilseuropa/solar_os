#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define RTC_BM8563_ADDRESS 0x51

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    bool clock_integrity;
} rtc_bm8563_datetime_t;

esp_err_t rtc_bm8563_init(void);
esp_err_t rtc_bm8563_get_datetime(rtc_bm8563_datetime_t *datetime);
esp_err_t rtc_bm8563_set_datetime(const rtc_bm8563_datetime_t *datetime);
bool rtc_bm8563_datetime_is_valid(const rtc_bm8563_datetime_t *datetime);
