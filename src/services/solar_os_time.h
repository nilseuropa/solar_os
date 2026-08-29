#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_TIMEZONE_NAME_MAX 32
#define SOLAR_OS_TIMEZONE_POSIX_MAX 80
#define SOLAR_OS_NTP_DEFAULT_SERVER "pool.ntp.org"
#define SOLAR_OS_NTP_DEFAULT_TIMEOUT_MS 15000U

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t weekday;
    bool clock_integrity;
} solar_os_datetime_t;

typedef esp_err_t (*solar_os_time_provider_get_fn_t)(void *user,
                                                      solar_os_datetime_t *datetime);
typedef esp_err_t (*solar_os_time_provider_set_fn_t)(void *user,
                                                      const solar_os_datetime_t *datetime);

typedef struct {
    solar_os_time_provider_get_fn_t get_utc_datetime;
    solar_os_time_provider_set_fn_t set_utc_datetime;
    void *user;
} solar_os_time_provider_t;

esp_err_t solar_os_time_init(void);
esp_err_t solar_os_time_register_provider(const char *owner,
                                          const solar_os_time_provider_t *provider);
esp_err_t solar_os_time_unregister_provider(const char *owner);
bool solar_os_time_has_provider(void);
uint64_t solar_os_time_uptime_ms(void);
void solar_os_time_format_uptime(uint64_t uptime_ms, char *buffer, size_t len);
esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *epoch_ms);
esp_err_t solar_os_time_get_datetime(solar_os_datetime_t *datetime);
esp_err_t solar_os_time_set_datetime(const solar_os_datetime_t *datetime);
esp_err_t solar_os_time_get_utc_datetime(solar_os_datetime_t *datetime);
esp_err_t solar_os_time_set_utc_datetime(const solar_os_datetime_t *datetime);
esp_err_t solar_os_time_utc_to_local(const solar_os_datetime_t *utc,
                                     solar_os_datetime_t *local);
esp_err_t solar_os_time_local_to_utc(const solar_os_datetime_t *local,
                                     solar_os_datetime_t *utc);
bool solar_os_time_datetime_is_valid(const solar_os_datetime_t *datetime);
void solar_os_time_get_timezone(char *name,
                                size_t name_len,
                                char *posix,
                                size_t posix_len);
esp_err_t solar_os_time_set_timezone(const char *timezone);
esp_err_t solar_os_time_ntp_sync(const char *server,
                                 uint32_t timeout_ms,
                                 solar_os_datetime_t *utc,
                                 solar_os_datetime_t *local);
