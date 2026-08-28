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
bool rtc_pcf85063_datetime_is_valid(const rtc_datetime_t *datetime);
