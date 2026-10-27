#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "solar_os_native_app_abi.h"

typedef struct { uint64_t last_cooperate_us; } foreground_state_t;
static foreground_state_t state;
static bool owner = true;
static uint64_t clock_us;
static unsigned delays, yielded_ticks, callbacks;
typedef unsigned TickType_t;
#define pdMS_TO_TICKS(ms) (ms)
static foreground_state_t *foreground_current(void) { return owner ? &state : NULL; }
static bool foreground_caller(void) { return owner; }
static uint64_t foreground_time_us(void) { return clock_us; }
static void vTaskDelay(TickType_t ticks)
{ ++delays; yielded_ticks += ticks; clock_us += ticks * 1000ULL; }
#include "foreground_cooperate.inc"
static void checkpoint(void) { ++callbacks; }

int main(void)
{
    for (unsigned ms = 0; ms < 50; ++ms) {
        clock_us = ms * 1000ULL;
        foreground_cooperate();
    }
    assert(!delays);
    clock_us = 50000;
    foreground_cooperate();
    assert(delays == 1 && yielded_ticks == 1 && state.last_cooperate_us == 51000);
    foreground_cooperate();
    assert(delays == 1);
    foreground_sleep_ms(8);
    assert(delays == 2 && yielded_ticks == 9 && state.last_cooperate_us == 59000);
    clock_us = 108999;
    foreground_cooperate();
    assert(delays == 2);
    clock_us = 109000;
    foreground_cooperate();
    assert(delays == 3 && yielded_ticks == 10);
    foreground_sleep_ms(0);
    assert(delays == 4 && yielded_ticks == 11);
    foreground_sleep_ms(5000);
    assert(delays == 5 && yielded_ticks == 1011);
    foreground_cooperate();
    assert(delays == 5); /* A timed wait already yielded. */
    owner = false;
    clock_us += 1000000;
    foreground_cooperate(); foreground_sleep_ms(10);
    assert(delays == 5);
    owner = true;

    solar_os_native_foreground_api_v1_t api = {
        .struct_size = offsetof(solar_os_native_foreground_api_v1_t, cooperate),
        .sleep_ms = foreground_sleep_ms, .cooperate = checkpoint,
    };
    solar_os_native_foreground_cooperate(&api);
    assert(delays == 6 && !callbacks); /* Legacy prefix uses timed sleep. */
    api.struct_size = sizeof(api);
    solar_os_native_foreground_cooperate(&api);
    assert(delays == 6 && callbacks == 1);
    puts("Foreground cooperation coalesces checkpoints and preserves timed waits and ownership");
}
