#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SOLAR_OS_CONTROLLER_TUNE_CYCLES_MAX 12U
typedef enum {
    SOLAR_OS_CONTROLLER_TUNE_IDLE,
    SOLAR_OS_CONTROLLER_TUNE_RUNNING,
    SOLAR_OS_CONTROLLER_TUNE_COMPLETE,
    SOLAR_OS_CONTROLLER_TUNE_ABORTED,
    SOLAR_OS_CONTROLLER_TUNE_FAILED,
} solar_os_controller_tune_state_t;
typedef enum {
    SOLAR_OS_CONTROLLER_TUNE_OK,
    SOLAR_OS_CONTROLLER_TUNE_TIMEOUT,
    SOLAR_OS_CONTROLLER_TUNE_UNSTABLE,
    SOLAR_OS_CONTROLLER_TUNE_BAD_SAMPLE,
    SOLAR_OS_CONTROLLER_TUNE_CLEANUP,
} solar_os_controller_tune_reason_t;
typedef struct {
    float setpoint, low, high, band, max_dt, timeout;
    uint32_t cycles;
    bool reverse;
} solar_os_controller_tune_config_t;
typedef struct {
    uint32_t state, reason, cycles, required_cycles, samples;
    float elapsed, amplitude, period, ultimate_gain;
    float kp, ki, kd, derivative_tau;
} solar_os_controller_tune_result_t;
typedef struct {
    solar_os_controller_tune_config_t config;
    solar_os_controller_tune_result_t result;
    double elapsed, last_rising, minimum, maximum;
    float periods[SOLAR_OS_CONTROLLER_TUNE_CYCLES_MAX];
    float amplitudes[SOLAR_OS_CONTROLLER_TUNE_CYCLES_MAX];
    float largest_dt;
    uint32_t warmup, next;
    bool initialized, high, saw_low;
} solar_os_controller_tuner_t;

bool solar_os_controller_tuner_init(solar_os_controller_tuner_t *t,
    const solar_os_controller_tune_config_t *config);
/* Pure relay identification; no allocation, clock, I/O or gain application.
 * The caller supplies dt and replaces the output with safe_output on completion
 * or failure. Two startup cycles are discarded before testing convergence. */
bool solar_os_controller_tuner_step(solar_os_controller_tuner_t *t,
    float measurement, float dt, float *output);
const char *solar_os_controller_tune_state_name(uint32_t state);
const char *solar_os_controller_tune_reason_name(uint32_t reason);
