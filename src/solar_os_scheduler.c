#include "solar_os_scheduler.h"

#include <limits.h>
#include <string.h>

#include "esp_timer.h"

void solar_os_tick_stats_reset(solar_os_tick_stats_t *stats)
{
    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
    }
}

bool solar_os_tick_due(solar_os_tick_stats_t *stats,
                       uint32_t configured_interval_ms,
                       uint32_t configured_deadline_ms,
                       uint32_t default_interval_ms,
                       uint32_t default_deadline_ms,
                       uint32_t now_ms)
{
    if (stats == NULL) {
        return false;
    }

    const uint32_t interval_ms = configured_interval_ms != 0 ?
        configured_interval_ms : default_interval_ms;
    const uint32_t deadline_ms = configured_deadline_ms != 0 ?
        configured_deadline_ms : default_deadline_ms;
    stats->interval_ms = interval_ms;
    stats->deadline_ms = deadline_ms;

    if (stats->dispatch_count != 0 &&
        (uint32_t)(now_ms - stats->last_dispatch_ms) < interval_ms) {
        return false;
    }
    stats->last_dispatch_ms = now_ms;
    return true;
}

int64_t solar_os_tick_begin(void)
{
    return esp_timer_get_time();
}

bool solar_os_tick_end(solar_os_tick_stats_t *stats, int64_t started_us)
{
    if (stats == NULL) {
        return false;
    }

    int64_t duration_us = esp_timer_get_time() - started_us;
    if (duration_us < 0) {
        duration_us = 0;
    }
    if (duration_us > UINT32_MAX) {
        duration_us = UINT32_MAX;
    }

    stats->dispatch_count++;
    stats->last_duration_us = (uint32_t)duration_us;
    if (stats->last_duration_us > stats->max_duration_us) {
        stats->max_duration_us = stats->last_duration_us;
    }

    const bool missed = stats->deadline_ms != 0 &&
        stats->last_duration_us > stats->deadline_ms * 1000ULL;
    if (missed) {
        stats->deadline_miss_count++;
    }
    return missed;
}

bool solar_os_tick_should_log_miss(const solar_os_tick_stats_t *stats)
{
    if (stats == NULL || stats->deadline_miss_count == 0) {
        return false;
    }
    const uint32_t count = stats->deadline_miss_count;
    return (count & (count - 1U)) == 0;
}
