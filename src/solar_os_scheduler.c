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

uint32_t solar_os_tick_interval_ms(uint32_t configured_interval_ms,
                                   uint32_t default_interval_ms)
{
    return configured_interval_ms != 0 ?
        configured_interval_ms : default_interval_ms;
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

    const uint32_t interval_ms =
        solar_os_tick_interval_ms(configured_interval_ms, default_interval_ms);
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

uint32_t solar_os_runtime_wait_ms(uint32_t requested_interval_ms,
                                  bool requires_fast_poll)
{
    const uint32_t maximum_ms = requires_fast_poll ?
        SOLAR_OS_RUNTIME_WAIT_POLL_MAX_MS : SOLAR_OS_RUNTIME_WAIT_EVENT_MAX_MS;
    if (requested_interval_ms == 0U) {
        return 1U;
    }
    return requested_interval_ms < maximum_ms ? requested_interval_ms : maximum_ms;
}

void solar_os_runtime_loop_note(solar_os_runtime_loop_stats_t *stats,
                                uint32_t now_ms,
                                uint32_t planned_wait_ms)
{
    if (stats == NULL) {
        return;
    }
    if (!stats->initialized) {
        memset(stats, 0, sizeof(*stats));
        stats->initialized = true;
        stats->sample_started_ms = now_ms;
        stats->planned_wait_min_ms = UINT32_MAX;
    }

    stats->loop_count++;
    stats->planned_wait_total_ms += planned_wait_ms;
    if (planned_wait_ms < stats->planned_wait_min_ms) {
        stats->planned_wait_min_ms = planned_wait_ms;
    }
    if (planned_wait_ms > stats->planned_wait_max_ms) {
        stats->planned_wait_max_ms = planned_wait_ms;
    }
}

bool solar_os_runtime_loop_take_report(solar_os_runtime_loop_stats_t *stats,
                                       uint32_t now_ms,
                                       uint32_t report_interval_ms,
                                       solar_os_runtime_loop_report_t *report)
{
    if (stats == NULL || report == NULL || !stats->initialized ||
        report_interval_ms == 0U) {
        return false;
    }

    const uint32_t elapsed_ms = now_ms - stats->sample_started_ms;
    if (elapsed_ms < report_interval_ms) {
        return false;
    }

    report->elapsed_ms = elapsed_ms;
    report->loop_count = stats->loop_count;
    report->planned_wait_total_ms = stats->planned_wait_total_ms;
    report->planned_wait_min_ms = stats->loop_count != 0U ?
        stats->planned_wait_min_ms : 0U;
    report->planned_wait_max_ms = stats->planned_wait_max_ms;

    memset(stats, 0, sizeof(*stats));
    stats->initialized = true;
    stats->sample_started_ms = now_ms;
    stats->planned_wait_min_ms = UINT32_MAX;
    return true;
}
