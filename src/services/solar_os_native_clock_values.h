#pragma once

#include "solar_os_native_clock_abi.h"
#include "solar_os_time.h"

static inline bool solar_os_native_datetime_in(const solar_os_native_datetime_v1_t *in,
                                                solar_os_datetime_t *out)
{
    if (!in || !out || in->year > UINT16_MAX || in->month > UINT8_MAX || in->day > UINT8_MAX ||
        in->hour > UINT8_MAX || in->minute > UINT8_MAX || in->second > UINT8_MAX ||
        in->weekday > UINT8_MAX || in->clock_integrity > 1) return false;
    *out = (solar_os_datetime_t){
        .year = in->year, .month = in->month, .day = in->day,
        .hour = in->hour, .minute = in->minute, .second = in->second,
        .weekday = in->weekday, .clock_integrity = in->clock_integrity != 0,
    };
    return true;
}

static inline void solar_os_native_datetime_out(const solar_os_datetime_t *in,
                                                 solar_os_native_datetime_v1_t *out)
{
    *out = (solar_os_native_datetime_v1_t){
        .year = in->year, .month = in->month, .day = in->day,
        .hour = in->hour, .minute = in->minute, .second = in->second,
        .weekday = in->weekday, .clock_integrity = in->clock_integrity,
    };
}
