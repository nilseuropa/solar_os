#pragma once
#include "solar_os_native_service_abi.h"
#define SOLAR_OS_NATIVE_CLOCK_SERVICE "clock"
#define SOLAR_OS_NATIVE_CLOCK_ABI 1U
#define SOLAR_OS_NATIVE_CLOCK_LOCAL 0U
#define SOLAR_OS_NATIVE_CLOCK_UTC 1U
typedef struct {
    uint32_t year, month, day, hour, minute, second, weekday, clock_integrity;
} solar_os_native_datetime_v1_t;
typedef struct {
    char name[32], posix[80];
} solar_os_native_timezone_v1_t;
typedef struct {
    const char *name;
    solar_os_native_handle_t library_lease;
    void *user;
    int (*get_utc)(void *user, solar_os_native_datetime_v1_t *datetime);
    int (*set_utc)(void *user, const solar_os_native_datetime_v1_t *datetime);
} solar_os_native_clock_provider_v1_t;

/* System clock and timezone operations, independent of the calling app.
 * The OS owns provider selection and timezone persistence. NTP uses the
 * supplied timeout. A provider retains its library lease until close. Its
 * callbacks can run on callers' tasks and must synchronize their own state.
 * If a callback is in flight, close returns BUSY and preserves ownership. */
typedef struct {
    uint32_t abi_version, struct_size;
    bool (*has_provider)(void);
    uint64_t (*uptime_ms)(void);
    void (*format_uptime)(uint64_t milliseconds, char *text, size_t capacity);
    int (*epoch_ms)(uint64_t *milliseconds);
    int (*get)(uint32_t zone, solar_os_native_datetime_v1_t *datetime);
    int (*set)(uint32_t zone, const solar_os_native_datetime_v1_t *datetime);
    int (*convert)(uint32_t destination_zone, const solar_os_native_datetime_v1_t *source,
        solar_os_native_datetime_v1_t *destination);
    bool (*valid)(const solar_os_native_datetime_v1_t *datetime);
    int (*get_timezone)(solar_os_native_timezone_v1_t *timezone);
    int (*set_timezone)(const char *timezone);
    int (*ntp_sync)(const char *server, uint32_t timeout_ms,
        solar_os_native_datetime_v1_t *utc, solar_os_native_datetime_v1_t *local);
    int (*provider_register)(const solar_os_native_clock_provider_v1_t *provider,
                             solar_os_native_handle_t *registration);
    int (*provider_close)(solar_os_native_handle_t registration);
    /* Resolve or inspect a timezone without changing the system clock. */
    bool (*resolve_timezone)(const char *value, char *name, size_t name_capacity,
        char *posix, size_t posix_capacity);
    bool (*timezone_is_raw_posix)(const char *value);
} solar_os_native_clock_api_v1_t;
