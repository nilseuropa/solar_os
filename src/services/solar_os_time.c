#include "solar_os_time.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "solar_os_board_caps.h"

#if SOLAR_OS_BOARD_HAS_RTC
#include "solar_os_board_rtc.h"
#endif

#define TIME_NVS_NAMESPACE "time"
#define TIME_NVS_TZ_NAME_KEY "tz_name"
#define TIME_NVS_TZ_POSIX_KEY "tz_posix"
#define TIME_NTP_SERVER_MAX 96
#define SECONDS_PER_DAY 86400LL

typedef struct {
    const char *alias;
    const char *name;
    const char *posix;
} timezone_alias_t;

static const timezone_alias_t timezone_aliases[] = {
    {"UTC", "UTC", "UTC0"},
    {"utc", "UTC", "UTC0"},
    {"Etc/UTC", "UTC", "UTC0"},
    {"Europe/Berlin", "Europe/Berlin", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"europe/berlin", "Europe/Berlin", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Berlin", "Europe/Berlin", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"berlin", "Europe/Berlin", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
};

static char timezone_name[SOLAR_OS_TIMEZONE_NAME_MAX] = "UTC";
static char timezone_posix[SOLAR_OS_TIMEZONE_POSIX_MAX] = "UTC0";
static bool timezone_loaded;

#if SOLAR_OS_BOARD_HAS_RTC
static void datetime_from_board(solar_os_datetime_t *out,
                                const solar_os_board_rtc_datetime_t *in)
{
    out->year = in->year;
    out->month = in->month;
    out->day = in->day;
    out->hour = in->hour;
    out->minute = in->minute;
    out->second = in->second;
    out->weekday = in->weekday;
    out->clock_integrity = in->clock_integrity;
}
#endif

static void datetime_from_tm(solar_os_datetime_t *out, const struct tm *in, bool clock_integrity)
{
    out->year = (uint16_t)(in->tm_year + 1900);
    out->month = (uint8_t)(in->tm_mon + 1);
    out->day = (uint8_t)in->tm_mday;
    out->hour = (uint8_t)in->tm_hour;
    out->minute = (uint8_t)in->tm_min;
    out->second = (uint8_t)in->tm_sec;
    out->weekday = (uint8_t)in->tm_wday;
    out->clock_integrity = clock_integrity;
}

#if SOLAR_OS_BOARD_HAS_RTC
static void datetime_to_board(solar_os_board_rtc_datetime_t *out,
                              const solar_os_datetime_t *in)
{
    out->year = in->year;
    out->month = in->month;
    out->day = in->day;
    out->hour = in->hour;
    out->minute = in->minute;
    out->second = in->second;
    out->weekday = in->weekday;
    out->clock_integrity = in->clock_integrity;
}
#endif

static bool datetime_is_valid(const solar_os_datetime_t *datetime)
{
    if (datetime == NULL) {
        return false;
    }

    if (datetime->year < 2000 ||
        datetime->month < 1 ||
        datetime->month > 12 ||
        datetime->day < 1 ||
        datetime->hour > 23 ||
        datetime->minute > 59 ||
        datetime->second > 59 ||
        datetime->weekday > 6) {
        return false;
    }

    static const uint8_t days_per_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    uint8_t days = days_per_month[datetime->month - 1U];
    const bool leap =
        (datetime->year % 4U == 0U && datetime->year % 100U != 0U) ||
        (datetime->year % 400U == 0U);
    if (datetime->month == 2 && leap) {
        days = 29;
    }
    return datetime->day <= days;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = ((153U * (month + (month > 2 ? (unsigned)-3 : 9U)) + 2U) / 5U) +
        day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (int64_t)era * 146097LL + (int64_t)doe - 719468LL;
}

static esp_err_t epoch_from_utc_datetime(const solar_os_datetime_t *datetime, time_t *epoch)
{
    if (!datetime_is_valid(datetime) || epoch == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t days = days_from_civil(datetime->year, datetime->month, datetime->day);
    const int64_t seconds = days * SECONDS_PER_DAY +
        ((int64_t)datetime->hour * 3600LL) +
        ((int64_t)datetime->minute * 60LL) +
        (int64_t)datetime->second;
    *epoch = (time_t)seconds;
    return ESP_OK;
}

static esp_err_t datetime_from_utc_epoch(time_t epoch,
                                         bool clock_integrity,
                                         solar_os_datetime_t *datetime)
{
    if (datetime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct tm utc_tm;
    if (gmtime_r(&epoch, &utc_tm) == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    datetime_from_tm(datetime, &utc_tm, clock_integrity);
    return datetime_is_valid(datetime) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static bool timezone_value_is_raw_posix(const char *timezone)
{
    bool has_digit = false;
    bool has_comma = false;
    bool has_slash = false;

    if (timezone == NULL || timezone[0] == '\0') {
        return false;
    }

    const size_t len = strlen(timezone);
    if (len >= SOLAR_OS_TIMEZONE_POSIX_MAX) {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)timezone; *p != '\0'; p++) {
        if (!isprint(*p) || isspace(*p)) {
            return false;
        }
        if (isdigit(*p)) {
            has_digit = true;
        } else if (*p == ',') {
            has_comma = true;
        } else if (*p == '/') {
            has_slash = true;
        }
    }

    return !has_slash || has_digit || has_comma;
}

static bool ntp_server_is_valid(const char *server)
{
    if (server == NULL || server[0] == '\0') {
        return false;
    }

    const size_t len = strlen(server);
    if (len >= TIME_NTP_SERVER_MAX) {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)server; *p != '\0'; p++) {
        if (!isprint(*p) || isspace(*p)) {
            return false;
        }
    }

    return true;
}

static bool timezone_resolve(const char *timezone,
                             char *name,
                             size_t name_len,
                             char *posix,
                             size_t posix_len)
{
    if (timezone == NULL || name == NULL || posix == NULL) {
        return false;
    }

    for (size_t i = 0; i < sizeof(timezone_aliases) / sizeof(timezone_aliases[0]); i++) {
        if (strcmp(timezone, timezone_aliases[i].alias) == 0) {
            strlcpy(name, timezone_aliases[i].name, name_len);
            strlcpy(posix, timezone_aliases[i].posix, posix_len);
            return true;
        }
    }

    if (!timezone_value_is_raw_posix(timezone)) {
        return false;
    }

    strlcpy(name, timezone, name_len);
    strlcpy(posix, timezone, posix_len);
    return true;
}

static void timezone_apply(void)
{
    setenv("TZ", timezone_posix, 1);
    tzset();
}

static void timezone_load(void)
{
    if (timezone_loaded) {
        return;
    }

    timezone_loaded = true;

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(TIME_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        timezone_apply();
        return;
    }

    char stored_name[SOLAR_OS_TIMEZONE_NAME_MAX];
    char stored_posix[SOLAR_OS_TIMEZONE_POSIX_MAX];
    size_t name_len = sizeof(stored_name);
    size_t posix_len = sizeof(stored_posix);
    ret = nvs_get_str(nvs, TIME_NVS_TZ_NAME_KEY, stored_name, &name_len);
    const esp_err_t posix_ret = nvs_get_str(nvs, TIME_NVS_TZ_POSIX_KEY, stored_posix, &posix_len);
    nvs_close(nvs);

    if (ret == ESP_OK && posix_ret == ESP_OK && timezone_value_is_raw_posix(stored_posix)) {
        strlcpy(timezone_name, stored_name, sizeof(timezone_name));
        strlcpy(timezone_posix, stored_posix, sizeof(timezone_posix));
    }
    timezone_apply();
}

static esp_err_t timezone_save(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(TIME_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs, TIME_NVS_TZ_NAME_KEY, timezone_name);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, TIME_NVS_TZ_POSIX_KEY, timezone_posix);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

esp_err_t solar_os_time_init(void)
{
    timezone_load();
#if !SOLAR_OS_BOARD_HAS_RTC
    return ESP_ERR_NOT_SUPPORTED;
#else
    return solar_os_board_rtc_init();
#endif
}

uint64_t solar_os_time_uptime_ms(void)
{
    const int64_t uptime_us = esp_timer_get_time();
    if (uptime_us <= 0) {
        return 0;
    }

    return (uint64_t)uptime_us / 1000ULL;
}

void solar_os_time_format_uptime(uint64_t uptime_ms, char *buffer, size_t len)
{
    if (buffer == NULL || len == 0) {
        return;
    }

    const uint64_t total_seconds = uptime_ms / 1000ULL;
    const uint64_t days = total_seconds / 86400ULL;
    const uint64_t hours = (total_seconds / 3600ULL) % 24ULL;
    const uint64_t minutes = (total_seconds / 60ULL) % 60ULL;
    const uint64_t seconds = total_seconds % 60ULL;

    if (days > 0) {
        snprintf(buffer,
                 len,
                 "%llud %02llu:%02llu:%02llu",
                 (unsigned long long)days,
                 (unsigned long long)hours,
                 (unsigned long long)minutes,
                 (unsigned long long)seconds);
    } else {
        snprintf(buffer,
                 len,
                 "%02llu:%02llu:%02llu",
                 (unsigned long long)hours,
                 (unsigned long long)minutes,
                 (unsigned long long)seconds);
    }
}

esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *epoch_ms)
{
    if (epoch_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *epoch_ms = 0;

    solar_os_datetime_t utc;
    esp_err_t ret = solar_os_time_get_utc_datetime(&utc);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!utc.clock_integrity) {
        return ESP_ERR_INVALID_STATE;
    }

    time_t epoch = 0;
    ret = epoch_from_utc_datetime(&utc, &epoch);
    if (ret != ESP_OK) {
        return ret;
    }

    *epoch_ms = (uint64_t)epoch * 1000ULL;
    return ESP_OK;
}

esp_err_t solar_os_time_get_datetime(solar_os_datetime_t *datetime)
{
    solar_os_datetime_t utc;
    esp_err_t ret = solar_os_time_get_utc_datetime(&utc);
    if (ret != ESP_OK) {
        return ret;
    }

    return solar_os_time_utc_to_local(&utc, datetime);
}

esp_err_t solar_os_time_set_datetime(const solar_os_datetime_t *datetime)
{
    solar_os_datetime_t utc;
    esp_err_t ret = solar_os_time_local_to_utc(datetime, &utc);
    if (ret != ESP_OK) {
        return ret;
    }

    return solar_os_time_set_utc_datetime(&utc);
}

esp_err_t solar_os_time_get_utc_datetime(solar_os_datetime_t *datetime)
{
    if (datetime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if !SOLAR_OS_BOARD_HAS_RTC
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_board_rtc_datetime_t board_datetime;
    const esp_err_t ret = solar_os_board_rtc_get_datetime(&board_datetime);
    if (ret != ESP_OK) {
        return ret;
    }

    datetime_from_board(datetime, &board_datetime);
    return ESP_OK;
#endif
}

esp_err_t solar_os_time_set_utc_datetime(const solar_os_datetime_t *datetime)
{
    if (datetime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if !SOLAR_OS_BOARD_HAS_RTC
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_board_rtc_datetime_t board_datetime;
    datetime_to_board(&board_datetime, datetime);
    return solar_os_board_rtc_set_datetime(&board_datetime);
#endif
}

esp_err_t solar_os_time_utc_to_local(const solar_os_datetime_t *utc, solar_os_datetime_t *local)
{
    timezone_load();

    time_t epoch = 0;
    esp_err_t ret = epoch_from_utc_datetime(utc, &epoch);
    if (ret != ESP_OK) {
        return ret;
    }

    struct tm local_tm;
    if (localtime_r(&epoch, &local_tm) == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    datetime_from_tm(local, &local_tm, utc->clock_integrity);
    return datetime_is_valid(local) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t solar_os_time_local_to_utc(const solar_os_datetime_t *local, solar_os_datetime_t *utc)
{
    if (!datetime_is_valid(local) || utc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    timezone_load();

    struct tm local_tm = {
        .tm_sec = local->second,
        .tm_min = local->minute,
        .tm_hour = local->hour,
        .tm_mday = local->day,
        .tm_mon = local->month - 1,
        .tm_year = local->year - 1900,
        .tm_isdst = -1,
    };

    const time_t epoch = mktime(&local_tm);
    if (epoch == (time_t)-1) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return datetime_from_utc_epoch(epoch, local->clock_integrity, utc);
}

bool solar_os_time_datetime_is_valid(const solar_os_datetime_t *datetime)
{
    return datetime_is_valid(datetime);
}

void solar_os_time_get_timezone(char *name,
                                size_t name_len,
                                char *posix,
                                size_t posix_len)
{
    timezone_load();

    if (name != NULL && name_len > 0) {
        strlcpy(name, timezone_name, name_len);
    }
    if (posix != NULL && posix_len > 0) {
        strlcpy(posix, timezone_posix, posix_len);
    }
}

esp_err_t solar_os_time_set_timezone(const char *timezone)
{
    char name[SOLAR_OS_TIMEZONE_NAME_MAX];
    char posix[SOLAR_OS_TIMEZONE_POSIX_MAX];

    if (!timezone_resolve(timezone, name, sizeof(name), posix, sizeof(posix))) {
        return ESP_ERR_INVALID_ARG;
    }

    timezone_loaded = true;
    strlcpy(timezone_name, name, sizeof(timezone_name));
    strlcpy(timezone_posix, posix, sizeof(timezone_posix));
    timezone_apply();
    return timezone_save();
}

esp_err_t solar_os_time_ntp_sync(const char *server,
                                 uint32_t timeout_ms,
                                 solar_os_datetime_t *utc,
                                 solar_os_datetime_t *local)
{
    const char *selected_server =
        server != NULL && server[0] != '\0' ? server : SOLAR_OS_NTP_DEFAULT_SERVER;
    if (!ntp_server_is_valid(selected_server)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (timeout_ms == 0) {
        timeout_ms = SOLAR_OS_NTP_DEFAULT_TIMEOUT_MS;
    }

    esp_netif_sntp_deinit();
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(selected_server);
    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    esp_netif_sntp_deinit();
    if (ret != ESP_OK) {
        return ret;
    }

    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }

    solar_os_datetime_t utc_datetime;
    ret = datetime_from_utc_epoch((time_t)tv.tv_sec, true, &utc_datetime);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = solar_os_time_set_utc_datetime(&utc_datetime);
    if (ret != ESP_OK) {
        return ret;
    }

    if (utc != NULL) {
        *utc = utc_datetime;
    }
    if (local != NULL) {
        ret = solar_os_time_utc_to_local(&utc_datetime, local);
    }
    return ret;
}
