#pragma once

/* Leave capability-heap space for drivers and SDK allocations. Large OS
 * buffers use the remaining contiguous region; zero opts out of this policy. */
#ifndef SOLAR_OS_PSRAM_GENERAL_RESERVE_BYTES
#define SOLAR_OS_PSRAM_GENERAL_RESERVE_BYTES (512U * 1024U)
#endif
#ifndef SOLAR_OS_PSRAM_BULK_MIN_BYTES
#define SOLAR_OS_PSRAM_BULK_MIN_BYTES (64U * 1024U)
#endif

#define SOLAR_OS_FOREGROUND_STACK_MAX_BYTES (28U * 1024U)
#define SOLAR_OS_TASK_INTERNAL_OVERHEAD_BYTES 1024U
#define SOLAR_OS_INTERNAL_RESERVE_BYTES (32U * 1024U)
#define SOLAR_OS_INTERNAL_LAUNCH_RESERVE_BYTES SOLAR_OS_INTERNAL_RESERVE_BYTES

#define SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(stack_bytes)                    \
    _Static_assert((stack_bytes) <= SOLAR_OS_FOREGROUND_STACK_MAX_BYTES,       \
                   "foreground worker exceeds the supported internal stack limit")
