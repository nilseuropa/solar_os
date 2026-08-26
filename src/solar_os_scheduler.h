#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SOLAR_OS_TICK_INTERVAL_DEFAULT_MS 25U
#define SOLAR_OS_TICK_DEADLINE_DEFAULT_MS 25U
#define SOLAR_OS_RUNTIME_WAIT_EVENT_MAX_MS 25U
#define SOLAR_OS_RUNTIME_WAIT_POLL_MAX_MS 10U

typedef struct {
    uint32_t interval_ms;
    uint32_t deadline_ms;
    uint32_t dispatch_count;
    uint32_t deadline_miss_count;
    uint32_t last_dispatch_ms;
    uint32_t last_duration_us;
    uint32_t max_duration_us;
} solar_os_tick_stats_t;

typedef struct {
    bool initialized;
    uint32_t sample_started_ms;
    uint32_t loop_count;
    uint64_t planned_wait_total_ms;
    uint32_t planned_wait_min_ms;
    uint32_t planned_wait_max_ms;
} solar_os_runtime_loop_stats_t;

typedef struct {
    uint32_t elapsed_ms;
    uint32_t loop_count;
    uint64_t planned_wait_total_ms;
    uint32_t planned_wait_min_ms;
    uint32_t planned_wait_max_ms;
} solar_os_runtime_loop_report_t;

void solar_os_tick_stats_reset(solar_os_tick_stats_t *stats);
uint32_t solar_os_tick_interval_ms(uint32_t configured_interval_ms,
                                   uint32_t default_interval_ms);
bool solar_os_tick_due(solar_os_tick_stats_t *stats,
                       uint32_t configured_interval_ms,
                       uint32_t configured_deadline_ms,
                       uint32_t default_interval_ms,
                       uint32_t default_deadline_ms,
                       uint32_t now_ms);
int64_t solar_os_tick_begin(void);
bool solar_os_tick_end(solar_os_tick_stats_t *stats, int64_t started_us);
bool solar_os_tick_should_log_miss(const solar_os_tick_stats_t *stats);
uint32_t solar_os_runtime_wait_ms(uint32_t requested_interval_ms,
                                  bool requires_fast_poll);
void solar_os_runtime_loop_note(solar_os_runtime_loop_stats_t *stats,
                                uint32_t now_ms,
                                uint32_t planned_wait_ms);
bool solar_os_runtime_loop_take_report(solar_os_runtime_loop_stats_t *stats,
                                       uint32_t now_ms,
                                       uint32_t report_interval_ms,
                                       solar_os_runtime_loop_report_t *report);
