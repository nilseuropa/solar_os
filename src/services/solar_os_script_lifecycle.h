#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef bool (*solar_os_script_stopped_fn)(void *user);

bool solar_os_script_wait_for_stop(solar_os_script_stopped_fn stopped,
                                   void *user,
                                   uint32_t timeout_ms,
                                   uint32_t poll_interval_ms);
