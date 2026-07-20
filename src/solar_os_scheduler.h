#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SOLAR_OS_TICK_INTERVAL_DEFAULT_MS 25U
#define SOLAR_OS_TICK_DEADLINE_DEFAULT_MS 25U

typedef struct {
    uint32_t interval_ms;
    uint32_t deadline_ms;
    uint32_t dispatch_count;
    uint32_t deadline_miss_count;
    uint32_t last_dispatch_ms;
    uint32_t last_duration_us;
    uint32_t max_duration_us;
} solar_os_tick_stats_t;

void solar_os_tick_stats_reset(solar_os_tick_stats_t *stats);
bool solar_os_tick_due(solar_os_tick_stats_t *stats,
                       uint32_t configured_interval_ms,
                       uint32_t configured_deadline_ms,
                       uint32_t default_interval_ms,
                       uint32_t default_deadline_ms,
                       uint32_t now_ms);
int64_t solar_os_tick_begin(void);
bool solar_os_tick_end(solar_os_tick_stats_t *stats, int64_t started_us);
bool solar_os_tick_should_log_miss(const solar_os_tick_stats_t *stats);
