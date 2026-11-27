#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_native_script_runner.h"
#include "solar_os_native_script_runner_abi.h"
#include "solar_os_script_runner.h"

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

static const solar_os_native_script_runner_api_v1_t *runner;

static int emit(solar_os_native_script_control_v1_t *control, void *user)
{
    (void)user;
    assert(!runner->should_cancel(control));
    const char raw[] = {'A', 0, 'B', 'C', 'D'};
    runner->output(control, raw, sizeof(raw));
    return SOLAR_OS_NATIVE_OK;
}

static int expire(solar_os_native_script_control_v1_t *control, void *user)
{
    (void)user;
    assert(!runner->should_cancel(control));
    fake_tick += 9;
    assert(runner->should_cancel(control)); /* Deadline across tick wrap. */
    return SOLAR_OS_NATIVE_OK;
}

static int report(solar_os_native_script_control_v1_t *control, void *user)
{
    (void)user;
    runner->report_error(control, SOLAR_OS_NATIVE_ERROR_BUSY, "interpreter already in use");
    return SOLAR_OS_NATIVE_OK;
}

static void test_runner(void)
{
    runner = solar_os_native_script_runner_get_service(SOLAR_OS_NATIVE_SCRIPT_RUNNER_SERVICE,
        SOLAR_OS_NATIVE_SCRIPT_RUNNER_ABI, sizeof(*runner));
    assert(runner && runner->struct_size == sizeof(*runner));
    char output[4];
    solar_os_native_script_options_v1_t options = {.output = output, .output_capacity = sizeof(output)};
    solar_os_native_script_result_v1_t result;
    assert(runner->run(&options, emit, NULL, &result) == SOLAR_OS_NATIVE_OK);
    assert(result.success && result.output_truncated && result.output_len == 3 &&
        !memcmp(output, "A\0B", 3) && !output[3]);
    assert(runner->run(&options, report, NULL, &result) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(!result.success && !strcmp(result.error, "interpreter already in use") && !result.output_len);

    reset_clock(UINT32_MAX - 4U);
    options.timeout_ms = 9;
    assert(runner->run(&options, expire, NULL, &result) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(result.timed_out && !result.success && !result.cancelled);

    stop_state_t state = {.started = fake_tick};
    options.cancelled = stopped;
    options.cancel_user = &state;
    assert(runner->run(&options, emit, NULL, &result) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(result.cancelled && !result.success && !result.timed_out && !output[0]);
    assert(runner->wait_stopped(stopped, &state, 20, 5));

    const solar_os_script_run_request_t invalid = {.input = "source"};
    solar_os_script_run_result_t raw_result;
    solar_os_script_run_control_t control;
    assert(solar_os_script_run_begin(&invalid, &raw_result, &control) == ESP_ERR_INVALID_ARG);
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
    test_runner();
    puts("script lifecycle and native runner tests: ok");
    return 0;
}
