#pragma once
#include "solar_os_native_service_abi.h"

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
} solar_os_native_controllers_api_v1_t;
