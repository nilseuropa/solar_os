#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"

/* Widen before multiplying since some FreeRTOS ports use TickType_t math. */
static inline TickType_t solar_os_script_ticks(uint32_t milliseconds)
{
    return (TickType_t)(((uint64_t)milliseconds * configTICK_RATE_HZ) / 1000U);
}

typedef bool (*solar_os_script_stopped_fn)(void *user);

bool solar_os_script_wait_for_stop(solar_os_script_stopped_fn stopped,
                                   void *user,
                                   uint32_t timeout_ms,
                                   uint32_t poll_interval_ms);
