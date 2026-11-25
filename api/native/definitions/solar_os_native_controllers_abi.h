#pragma once
#include "solar_os_native_service_abi.h"
#include "solar_os_native_motion_abi.h"

#define SOLAR_OS_NATIVE_CONTROLLERS_SERVICE "controllers"
#define SOLAR_OS_NATIVE_CONTROLLERS_ABI 1U

typedef struct {
    float kp, ki, kd, derivative_tau;
    float output_min, output_max, max_dt, setpoint_min, setpoint_max;
    uint32_t period_ms, timeout_ms;
} solar_os_native_controller_config_v1_t;

typedef struct {
    float setpoint, measurement, dt, feedforward;
    uint64_t sample_us; /* Host monotonic acquisition time. */
    uint32_t run; /* Zero monitors feedback and resets PID while waiting. */
} solar_os_native_controller_sample_v1_t;

typedef struct {
    float output, p, i, d; /* Output includes feedforward and the configured cap. */
    uint32_t saturated;
} solar_os_native_controller_output_v1_t;

/* Bound controllers use the same OS stream/PID/output path as the shell.
 * Endpoint settings are temporary and restored before releasing ownership. */
typedef struct {
    solar_os_native_controller_config_v1_t pid;
    char motor[20], encoder[20];
    solar_os_native_encoder_config_v1_t encoder_config;
    solar_os_native_motor_config_v1_t motor_config;
    float acceleration_limit, velocity_deadband;
    float friction_bias_forward, friction_bias_reverse;
    uint32_t command_timeout_ms;
} solar_os_native_bound_controller_config_v1_t;
typedef struct {
    uint32_t enabled, claimed, waiting, faulted, command_valid, steps, faults, saturated;
    int32_t last_error;
    uint64_t command_expires_us;
    float target, setpoint, output, p, i, d, feedforward;
    solar_os_native_encoder_feedback_v1_t feedback;
} solar_os_native_bound_controller_status_v1_t;

/* OS-owned, explicitly stepped PID instances in the common controller registry.
 * The caller owns its sampling clock and actuator handling. No callbacks or
 * borrowed pointers are retained. Instances start waiting; evaluate runs the
 * OS PID synchronously on the caller's task. Generic controller stop revokes
 * evaluation; the owner must observe failure and stop its actuator. Only the
 * owner handle may pause, fault or close an instance. Retain it until physical
 * output cleanup succeeds, and close before unloading the owner module. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*open)(const char *name, const char *owner, const solar_os_native_controller_config_v1_t *config, uint32_t *controller);
    int (*evaluate)(uint32_t controller, const solar_os_native_controller_sample_v1_t *sample, solar_os_native_controller_output_v1_t *out);
    int (*pause)(uint32_t controller);
    int (*fault)(uint32_t controller, int error);
    int (*close)(uint32_t controller);
    /* Optional append-only bound endpoint extension. A failed open may return
     * a nonzero handle when rollback is incomplete: retain it and retry close.
     * Command timestamps are original acquisitions on the host uptime clock. */
    int (*open_bound)(const char *name,const char *owner,
        const solar_os_native_bound_controller_config_v1_t *config,uint32_t *controller);
    int (*command)(uint32_t controller,float setpoint,uint64_t acquired_us);
    int (*step_bound)(uint32_t controller,solar_os_native_bound_controller_status_v1_t *status);
    int (*status_bound)(uint32_t controller,solar_os_native_bound_controller_status_v1_t *status);
} solar_os_native_controllers_api_v1_t;
