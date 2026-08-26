#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "freertos/task.h"
#include "solar_os_script_lifecycle.h"

typedef struct {
    TickType_t started;
    TickType_t stop_after;
} stop_state_t;

static TickType_t fake_tick;
static size_t delay_count;

TickType_t xTaskGetTickCount(void)
{
    return fake_tick;
}

void vTaskDelay(TickType_t ticks)
{
    fake_tick += ticks;
    delay_count++;
}

static bool stopped(void *user)
{
    const stop_state_t *state = user;
    return (fake_tick - state->started) >= state->stop_after;
}

static void reset_clock(TickType_t tick)
{
    fake_tick = tick;
    delay_count = 0U;
}

int main(void)
{
    stop_state_t state = {.started = 100U, .stop_after = 0U};
    reset_clock(state.started);
    assert(solar_os_script_wait_for_stop(stopped, &state, 100U, 20U));
    assert(delay_count == 0U);

    state = (stop_state_t){.started = 200U, .stop_after = 30U};
    reset_clock(state.started);
    assert(solar_os_script_wait_for_stop(stopped, &state, 100U, 20U));
    assert(fake_tick == 240U);
    assert(delay_count == 2U);

    state = (stop_state_t){.started = 300U, .stop_after = 100U};
    reset_clock(state.started);
    assert(!solar_os_script_wait_for_stop(stopped, &state, 50U, 20U));
    assert(fake_tick == 350U);
    assert(delay_count == 3U);

    state = (stop_state_t){.started = UINT32_MAX - 5U, .stop_after = 8U};
    reset_clock(state.started);
    assert(solar_os_script_wait_for_stop(stopped, &state, 20U, 4U));
    assert(delay_count == 2U);

    assert(!solar_os_script_wait_for_stop(NULL, NULL, 100U, 20U));
    puts("script lifecycle tests: ok");
    return 0;
}
