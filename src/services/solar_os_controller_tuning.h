#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "solar_os_controller_math.h"

#define SOLAR_OS_CONTROLLER_TUNE_CYCLES_MAX 12U
#define SOLAR_OS_CONTROLLER_TUNE_SAMPLES_MAX 256U
typedef struct {
    solar_os_controller_tune_config_t config;
    solar_os_controller_tune_result_t result;
    double elapsed, last_rising, minimum, maximum;
    float periods[SOLAR_OS_CONTROLLER_TUNE_CYCLES_MAX];
    float amplitudes[SOLAR_OS_CONTROLLER_TUNE_CYCLES_MAX];
    float largest_dt;
    uint32_t warmup, next;
    bool initialized, high, saw_low;
    /* Bounded step-response storage, ported from Tortuga's PidTuner. All
     * orchestration is per controller; no robot/joint/device code lives here. */
    float times[SOLAR_OS_CONTROLLER_TUNE_SAMPLES_MAX];
    float values[SOLAR_OS_CONTROLLER_TUNE_SAMPLES_MAX];
    uint32_t count;
    double phase_started, window_time, window_sum, window_square;
    uint32_t window_count;
    float baseline, threshold, initial, step_drive, direction, limit;
    float step_targets[SOLAR_OS_CONTROLLER_TUNE_CYCLES_MAX];
    float kp_sum, ki_sum, kd_sum, candidate_kp, candidate_ki, candidate_kd;
    uint32_t valid_steps, stable_windows, validation_point;
    solar_os_controller_math_state_t validation;
} solar_os_controller_tuner_t;

bool solar_os_controller_tuner_init(solar_os_controller_tuner_t *t,
    const solar_os_controller_tune_config_t *config);
/* Allocation-free experiment; no clock, I/O or gain application. Automatic
 * mode finds motion, fits multiple steps and validates the averaged candidate.
 * Relay mode discards two startup cycles before testing convergence. The caller
 * supplies dt and replaces output with safe_output on completion or failure. */
bool solar_os_controller_tuner_step(solar_os_controller_tuner_t *t,
    float measurement, float dt, float *output);
const char *solar_os_controller_tune_state_name(uint32_t state);
const char *solar_os_controller_tune_reason_name(uint32_t reason);
const char *solar_os_controller_tune_phase_name(uint32_t phase);
