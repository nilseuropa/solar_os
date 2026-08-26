#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "solar_os_scheduler.h"

static int64_t fake_time_us;

int64_t esp_timer_get_time(void)
{
    return fake_time_us;
}

static void test_runtime_wait(void)
{
    assert(solar_os_runtime_wait_ms(100U, false) == 25U);
    assert(solar_os_runtime_wait_ms(100U, true) == 10U);
    assert(solar_os_runtime_wait_ms(5U, false) == 5U);
    assert(solar_os_runtime_wait_ms(5U, true) == 5U);
    assert(solar_os_runtime_wait_ms(0U, false) == 1U);
}

static void test_runtime_report(void)
{
    solar_os_runtime_loop_stats_t stats = {0};
    solar_os_runtime_loop_report_t report = {0};

    solar_os_runtime_loop_note(&stats, 100U, 25U);
    solar_os_runtime_loop_note(&stats, 125U, 10U);
    solar_os_runtime_loop_note(&stats, 150U, 5U);
    assert(!solar_os_runtime_loop_take_report(&stats, 199U, 100U, &report));
    assert(solar_os_runtime_loop_take_report(&stats, 200U, 100U, &report));
    assert(report.elapsed_ms == 100U);
    assert(report.loop_count == 3U);
    assert(report.planned_wait_total_ms == 40U);
    assert(report.planned_wait_min_ms == 5U);
    assert(report.planned_wait_max_ms == 25U);

    solar_os_runtime_loop_note(&stats, 205U, 7U);
    assert(solar_os_runtime_loop_take_report(&stats, 300U, 100U, &report));
    assert(report.loop_count == 1U);
    assert(report.planned_wait_min_ms == 7U);
    assert(report.planned_wait_max_ms == 7U);
}

static void test_tick_stats(void)
{
    solar_os_tick_stats_t stats = {0};
    assert(solar_os_tick_due(&stats, 0U, 0U, 25U, 25U, 10U));
    fake_time_us = 1000;
    const int64_t started = solar_os_tick_begin();
    fake_time_us = 27001;
    assert(solar_os_tick_end(&stats, started));
    assert(stats.dispatch_count == 1U);
    assert(stats.deadline_miss_count == 1U);
    assert(solar_os_tick_should_log_miss(&stats));
}

int main(void)
{
    test_runtime_wait();
    test_runtime_report();
    test_tick_stats();
    puts("scheduler tests: ok");
    return 0;
}
