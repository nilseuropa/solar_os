#pragma once
#include <stdint.h>

#define SOLAR_OS_NATIVE_TIME_SERVICE "time"
#define SOLAR_OS_NATIVE_TIME_ABI 1U

/* Monotonic microseconds and cooperative sleep on the calling task. */
typedef struct {
    uint32_t abi_version, struct_size;
    int64_t (*now_us)(void);
    void (*sleep_ms)(uint32_t milliseconds);
} solar_os_native_time_api_v1_t;
