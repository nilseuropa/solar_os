#pragma once

#define SOLAR_OS_FOREGROUND_STACK_MAX_BYTES (28U * 1024U)
#define SOLAR_OS_TASK_INTERNAL_OVERHEAD_BYTES 1024U
#define SOLAR_OS_INTERNAL_RESERVE_BYTES (32U * 1024U)
#define SOLAR_OS_INTERNAL_LAUNCH_RESERVE_BYTES SOLAR_OS_INTERNAL_RESERVE_BYTES

#define SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(stack_bytes)                    \
    _Static_assert((stack_bytes) <= SOLAR_OS_FOREGROUND_STACK_MAX_BYTES,       \
                   "foreground worker exceeds the supported internal stack limit")
