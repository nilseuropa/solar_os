#ifndef SOLAR_OS_TIMEZONE_H
#define SOLAR_OS_TIMEZONE_H

#include <stdbool.h>
#include <stddef.h>

/* Validate an already resolved POSIX TZ value without changing its sign. */
bool solar_os_timezone_value_is_raw_posix(const char *timezone);

/* Resolve user-facing UTC offsets and named aliases to POSIX TZ syntax. */
bool solar_os_timezone_resolve(const char *timezone,
                               char *name,
                               size_t name_len,
                               char *posix,
                               size_t posix_len);

#endif
