#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "solar_os_timezone.h"

static void expect_timezone(const char *input,
                            const char *expected_name,
                            const char *expected_posix)
{
    char name[32];
    char posix[80];
    assert(solar_os_timezone_resolve(input, name, sizeof(name), posix, sizeof(posix)));
    assert(strcmp(name, expected_name) == 0);
    assert(strcmp(posix, expected_posix) == 0);
}

static void expect_rejected(const char *input)
{
    char name[32];
    char posix[80];
    assert(!solar_os_timezone_resolve(input, name, sizeof(name), posix, sizeof(posix)));
}

static void expect_local_time(const char *input,
                              int expected_day,
                              int expected_hour,
                              int expected_minute)
{
    char name[32];
    char posix[80];
    assert(solar_os_timezone_resolve(input, name, sizeof(name), posix, sizeof(posix)));
    assert(setenv("TZ", posix, 1) == 0);
    tzset();

    const time_t epoch = 86400;
    struct tm local;
    assert(localtime_r(&epoch, &local) != NULL);
    assert(local.tm_mday == expected_day);
    assert(local.tm_hour == expected_hour);
    assert(local.tm_min == expected_minute);
}

int main(void)
{
    expect_timezone("UTC", "UTC", "UTC0");
    expect_timezone("utc", "UTC", "UTC0");
    expect_timezone("UTC-8", "UTC-8", "UTC+8");
    expect_timezone("UTC+8", "UTC+8", "UTC-8");
    expect_timezone("utc+05:30", "UTC+05:30", "UTC-05:30");
    expect_timezone("UTC-03:30:15", "UTC-03:30:15", "UTC+03:30:15");
    expect_timezone("Europe/Berlin", "Europe/Berlin",
                    "CET-1CEST,M3.5.0/2,M10.5.0/3");
    expect_timezone("PST8PDT,M3.2.0,M11.1.0",
                    "PST8PDT,M3.2.0,M11.1.0",
                    "PST8PDT,M3.2.0,M11.1.0");

    expect_local_time("UTC-8", 1, 16, 0);
    expect_local_time("UTC+8", 2, 8, 0);
    expect_local_time("UTC+05:30", 2, 5, 30);

    expect_rejected("UTC+");
    expect_rejected("UTC+1:2");
    expect_rejected("UTC+25");
    expect_rejected("UTC+24:01");
    expect_rejected("UTC+1:60");
    expect_rejected("UTC+1:00:60");
    expect_rejected("UTC+1x");
    expect_rejected("America/Los_Angeles");

    puts("time timezone tests passed");
    return 0;
}
