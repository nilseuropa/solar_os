#include "solar_os_board_rtc.h"

#include "rtc_pcf8563.h"

static rtc_pcf8563_t board_rtc;

static void rtc_datetime_from_driver(solar_os_board_rtc_datetime_t *out,
                                     const rtc_pcf8563_datetime_t *in)
{
    out->year = in->year;
    out->month = in->month;
    out->day = in->day;
    out->weekday = in->weekday;
    out->hour = in->hour;
    out->minute = in->minute;
    out->second = in->second;
    out->clock_integrity = in->clock_integrity;
}

static void rtc_datetime_to_driver(rtc_pcf8563_datetime_t *out,
                                   const solar_os_board_rtc_datetime_t *in)
{
    out->year = in->year;
    out->month = in->month;
    out->day = in->day;
    out->weekday = in->weekday;
    out->hour = in->hour;
    out->minute = in->minute;
    out->second = in->second;
    out->clock_integrity = in->clock_integrity;
}

esp_err_t solar_os_board_rtc_init(void)
{
    return rtc_pcf8563_init_device(&board_rtc, "i2c0", RTC_PCF8563_ADDRESS);
}

esp_err_t solar_os_board_rtc_get_datetime(solar_os_board_rtc_datetime_t *datetime)
{
    if (datetime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    rtc_pcf8563_datetime_t driver_datetime;
    const esp_err_t err = rtc_pcf8563_get_datetime_device(&board_rtc, &driver_datetime);
    if (err != ESP_OK) {
        return err;
    }

    rtc_datetime_from_driver(datetime, &driver_datetime);
    return ESP_OK;
}

esp_err_t solar_os_board_rtc_set_datetime(const solar_os_board_rtc_datetime_t *datetime)
{
    if (datetime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    rtc_pcf8563_datetime_t driver_datetime;
    rtc_datetime_to_driver(&driver_datetime, datetime);
    return rtc_pcf8563_set_datetime_device(&board_rtc, &driver_datetime);
}
