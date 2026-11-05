#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "solar_os_controller_math.h"
#include "solar_os_controller_tuning.h"
#include "solar_os_parameters.h"
#include "solar_os_stream.h"
#include "solar_os_native_controllers_abi.h"

#define SOLAR_OS_CONTROLLER_MAX 8U
#define SOLAR_OS_CONTROLLER_NAME_MAX 24U
#define SOLAR_OS_CONTROLLER_TARGET_MAX (SOLAR_OS_PARAMETER_PATH_MAX + 12U)

typedef struct {
    char name[SOLAR_OS_CONTROLLER_NAME_MAX];
    solar_os_controller_math_config_t algorithm;
    float setpoint, setpoint_min, setpoint_max, safe_output;
    uint32_t period_ms, timeout_ms, frequency_hz;
    uint32_t command_timeout_ms;
    float acceleration_limit, velocity_deadband, friction_bias_forward, friction_bias_reverse;
    char source[SOLAR_OS_STREAM_ID_MAX]; /* Empty: timestamped feed() input. */
    char target[SOLAR_OS_CONTROLLER_TARGET_MAX]; /* parameter:path or pwm:provider:output */
} solar_os_controller_config_t;

typedef struct {
    solar_os_controller_config_t config;
    solar_os_controller_math_state_t state;
    bool enabled, claimed, faulted, has_measurement;
    float measurement;
    uint64_t sample_ms;
    uint32_t steps, faults;
    esp_err_t last_error;
    char owner[16]; /* Nonempty: explicitly stepped instance; owner keeps its handle. */
    bool waiting;
    float feedforward;
    solar_os_controller_tune_result_t tuning;
    bool command_valid;
    uint64_t command_expires_us;
    float active_setpoint;
    solar_os_native_encoder_feedback_v1_t feedback;
} solar_os_controller_info_t;

esp_err_t solar_os_controller_create(const char *name, solar_os_controller_kind_t kind);
esp_err_t solar_os_controller_delete(const char *name);
/* Standalone PID only, stopped and unclaimed. band=0 uses hysteresis/2.
 * Bound instances use the normal controllers job; unbound/manual instances
 * use step(). Gains are reported in status and never applied automatically. */
esp_err_t solar_os_controller_tune(const char *name, float band, uint32_t cycles, uint32_t timeout_ms);
size_t solar_os_controller_count(void);
bool solar_os_controller_get_info(size_t index, solar_os_controller_info_t *info);
esp_err_t solar_os_controller_find(const char *name, solar_os_controller_info_t *info);
/* Numeric configuration. Only setpoint may change while an output is claimed;
 * all configuration is frozen during tuning. */
esp_err_t solar_os_controller_configure(const char *name, const char *key, float value);
esp_err_t solar_os_controller_bind(const char *name, const char *source, const char *target);
/* Feed timestamps use the same monotonic uptime clock as tick(). */
esp_err_t solar_os_controller_feed(const char *name, float measurement, uint64_t sample_ms);
/* Unbound/manual stepping never writes an actuator. */
esp_err_t solar_os_controller_step(const char *name, float measurement, float dt, float *output);
esp_err_t solar_os_controller_manual(const char *name, float output);
esp_err_t solar_os_controller_auto(const char *name);
esp_err_t solar_os_controller_reset(const char *name);
/* start claims the output but makes no write. Faults latch until stop/start;
 * stop writes safe_output before releasing ownership. Failed cleanup is retryable. */
esp_err_t solar_os_controller_start(const char *name);
esp_err_t solar_os_controller_stop(const char *name);
esp_err_t solar_os_controllers_stop_all(void);
/* Called by one worker. Endpoint callbacks must not re-enter this service. */
void solar_os_controllers_tick(uint64_t now_ms);

/* Owned PID instances share the registry and algorithm implementation. Their
 * sampling owner calls evaluate with the host acquisition/execution clock;
 * the generic job must not step them a second time. No hardware is accessed. */
esp_err_t solar_os_controller_open_owned(const char *name, const char *owner,
    const solar_os_native_controller_config_v1_t *config, uint32_t *id);
esp_err_t solar_os_controller_evaluate_owned(uint32_t id,
    const solar_os_native_controller_sample_v1_t *sample, uint64_t now_us,
    solar_os_native_controller_output_v1_t *out);
esp_err_t solar_os_controller_pause_owned(uint32_t id);
esp_err_t solar_os_controller_fault_owned(uint32_t id, esp_err_t error);
esp_err_t solar_os_controller_close_owned(uint32_t id);
esp_err_t solar_os_controller_open_bound(const char *name,const char *owner,
    const solar_os_native_bound_controller_config_v1_t *config,uint32_t *id);
esp_err_t solar_os_controller_command_owned(uint32_t id,float setpoint,uint64_t acquired_us);
esp_err_t solar_os_controller_step_bound(uint32_t id,solar_os_native_bound_controller_status_v1_t *status);
esp_err_t solar_os_controller_status_bound(uint32_t id,solar_os_native_bound_controller_status_v1_t *status);
