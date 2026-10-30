#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "solar_os_work.h"

typedef unsigned TickType_t;
#define pdMS_TO_TICKS(ms) ((ms) * TEST_TICK_HZ / 1000U)
static uint64_t now_us;
static unsigned sleeps, slept_ticks;
static int64_t esp_timer_get_time(void) { return (int64_t)now_us; }
static void vTaskDelay(TickType_t ticks)
{
    assert(ticks >= 2U);
    sleeps++;
    slept_ticks += ticks;
    now_us += (uint64_t)ticks * 1000000U / TEST_TICK_HZ;
}
#include "work_budget.inc"

int main(void)
{
    solar_os_work_budget_t first, second;
    solar_os_work_begin(&first);
    for (unsigned i = 0; i < 100; ++i) solar_os_work_checkpoint(&first);
    assert(!sleeps);
    now_us = SOLAR_OS_WORK_SLICE_US - 1U;
    solar_os_work_checkpoint(&first);
    assert(!sleeps);
    now_us++;
    solar_os_work_checkpoint(&first);
    assert(sleeps == 1 && first.last_yield_us == now_us);
    const unsigned minimum = pdMS_TO_TICKS(1U);
    assert(slept_ticks == (minimum ? minimum : 1U) + 1U);
    solar_os_work_checkpoint(&first);
    assert(sleeps == 1); /* Time spent sleeping starts the next budget. */

    now_us += SOLAR_OS_WORK_SLICE_US / 2U;
    solar_os_work_begin(&second);
    now_us += SOLAR_OS_WORK_SLICE_US / 2U;
    solar_os_work_checkpoint(&second);
    assert(sleeps == 1);
    solar_os_work_checkpoint(&first);
    assert(sleeps == 2); /* Another operation cannot reset this budget. */
    now_us += 1000000U;
    solar_os_work_checkpoint(&second);
    assert(sleeps == 3 && second.last_yield_us == now_us);
    puts("work budget: elapsed-time pacing, full blocked tick, independent state passed");
}
