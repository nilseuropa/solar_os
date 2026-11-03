#pragma once

#include <stdbool.h>

/* Portable, allocation-free controller algorithms. All times are seconds;
 * setpoint and measurement share a unit, output uses the actuator's unit. */
typedef enum {
    SOLAR_OS_CONTROLLER_PID,
    SOLAR_OS_CONTROLLER_HYSTERESIS,
} solar_os_controller_kind_t;

typedef struct {
    solar_os_controller_kind_t kind;
    float kp, ki, kd;
    float derivative_tau;
    float output_min, output_max;
    float hysteresis; /* Full width of the band around the setpoint. */
    float max_dt;
    bool reverse; /* Increasing output decreases the measured value. */
} solar_os_controller_math_config_t;

typedef struct {
    bool initialized, automatic, tracking, saturated;
    float previous_measurement;
    float error, p, i, d, output;
} solar_os_controller_math_state_t;

solar_os_controller_math_config_t solar_os_controller_math_defaults(
    solar_os_controller_kind_t kind);
bool solar_os_controller_math_valid(const solar_os_controller_math_config_t *config);
bool solar_os_controller_math_reset(const solar_os_controller_math_config_t *config,
    solar_os_controller_math_state_t *state, float output);
bool solar_os_controller_math_manual(const solar_os_controller_math_config_t *config,
    solar_os_controller_math_state_t *state, float output);
void solar_os_controller_math_auto(solar_os_controller_math_state_t *state);
/* Invalid input, arithmetic overflow, or dt outside (0,max_dt] returns false
 * without changing state. The caller must choose its fault output policy. */
bool solar_os_controller_math_step(const solar_os_controller_math_config_t *config,
    solar_os_controller_math_state_t *state, float setpoint, float measurement,
    float dt);
