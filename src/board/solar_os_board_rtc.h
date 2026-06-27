#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    bool clock_integrity;
} solar_os_board_rtc_datetime_t;

esp_err_t solar_os_board_rtc_init(void);
esp_err_t solar_os_board_rtc_get_datetime(solar_os_board_rtc_datetime_t *datetime);
esp_err_t solar_os_board_rtc_set_datetime(const solar_os_board_rtc_datetime_t *datetime);

