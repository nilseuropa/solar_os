#include "solar_os_timezone.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

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

static bool timezone_has_utc_prefix(const char *timezone)
{
    return timezone != NULL &&
        (timezone[0] == 'U' || timezone[0] == 'u') &&
        (timezone[1] == 'T' || timezone[1] == 't') &&
        (timezone[2] == 'C' || timezone[2] == 'c');
}

static bool timezone_parse_offset_component(const char **cursor, unsigned *value)
{
    const unsigned char *p = (const unsigned char *)*cursor;
    if (!isdigit(p[0]) || !isdigit(p[1])) {
        return false;
    }

    *value = (unsigned)(p[0] - '0') * 10U + (unsigned)(p[1] - '0');
    *cursor += 2;
    return true;
}

static bool timezone_utc_offset_is_valid(const char *offset)
{
    const char *cursor = offset;
    unsigned hours = 0;
    unsigned minutes = 0;
    unsigned seconds = 0;

    if (cursor == NULL || (*cursor != '+' && *cursor != '-')) {
        return false;
    }
    cursor++;

    if (!isdigit((unsigned char)*cursor)) {
        return false;
    }
    hours = (unsigned)(*cursor - '0');
    cursor++;
    if (isdigit((unsigned char)*cursor)) {
        hours = hours * 10U + (unsigned)(*cursor - '0');
        cursor++;
    }

    if (*cursor == ':') {
        cursor++;
        if (!timezone_parse_offset_component(&cursor, &minutes)) {
            return false;
        }
        if (*cursor == ':') {
            cursor++;
            if (!timezone_parse_offset_component(&cursor, &seconds)) {
                return false;
            }
        }
    }

    return *cursor == '\0' &&
        hours <= 24U &&
        minutes <= 59U &&
        seconds <= 59U &&
        (hours < 24U || (minutes == 0U && seconds == 0U));
}

static bool timezone_resolve_utc_offset(const char *timezone,
                                        char *name,
                                        size_t name_len,
                                        char *posix,
                                        size_t posix_len)
{
    if (!timezone_has_utc_prefix(timezone) ||
        !timezone_utc_offset_is_valid(timezone + 3)) {
        return false;
    }

    const char posix_sign = timezone[3] == '+' ? '-' : '+';
    const int name_written = snprintf(name, name_len, "UTC%s", timezone + 3);
    const int posix_written = snprintf(posix, posix_len, "UTC%c%s", posix_sign, timezone + 4);
    return name_written >= 0 && (size_t)name_written < name_len &&
        posix_written >= 0 && (size_t)posix_written < posix_len;
}

bool solar_os_timezone_value_is_raw_posix(const char *timezone)
{
    bool has_digit = false;
    bool has_comma = false;
    bool has_slash = false;

    if (timezone == NULL || timezone[0] == '\0') {
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

bool solar_os_timezone_resolve(const char *timezone,
                               char *name,
                               size_t name_len,
                               char *posix,
                               size_t posix_len)
{
    if (timezone == NULL || name == NULL || name_len == 0 ||
        posix == NULL || posix_len == 0) {
        return false;
    }

    for (size_t i = 0; i < sizeof(timezone_aliases) / sizeof(timezone_aliases[0]); i++) {
        if (strcmp(timezone, timezone_aliases[i].alias) == 0) {
            const int name_written = snprintf(name, name_len, "%s", timezone_aliases[i].name);
            const int posix_written = snprintf(posix, posix_len, "%s", timezone_aliases[i].posix);
            return name_written >= 0 && (size_t)name_written < name_len &&
                posix_written >= 0 && (size_t)posix_written < posix_len;
        }
    }

    if (timezone_resolve_utc_offset(timezone, name, name_len, posix, posix_len)) {
        return true;
    }
    if (timezone_has_utc_prefix(timezone) &&
        (timezone[3] == '+' || timezone[3] == '-')) {
        return false;
    }
    if (!solar_os_timezone_value_is_raw_posix(timezone)) {
        return false;
    }

    const int name_written = snprintf(name, name_len, "%s", timezone);
    const int posix_written = snprintf(posix, posix_len, "%s", timezone);
    return name_written >= 0 &&
        posix_written >= 0 && (size_t)posix_written < posix_len;
}
