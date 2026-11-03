#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "solar_os_controller_math.h"
#include "solar_os_parameters.h"
#include "solar_os_stream.h"

#define SOLAR_OS_CONTROLLER_MAX 8U
#define SOLAR_OS_CONTROLLER_NAME_MAX 20U
#define SOLAR_OS_CONTROLLER_TARGET_MAX (SOLAR_OS_PARAMETER_PATH_MAX + 12U)

typedef struct {
    char name[SOLAR_OS_CONTROLLER_NAME_MAX];
    solar_os_controller_math_config_t algorithm;
    float setpoint, setpoint_min, setpoint_max, safe_output;
    uint32_t period_ms, timeout_ms, frequency_hz;
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
} solar_os_controller_info_t;

esp_err_t solar_os_controller_create(const char *name, solar_os_controller_kind_t kind);
esp_err_t solar_os_controller_delete(const char *name);
size_t solar_os_controller_count(void);
bool solar_os_controller_get_info(size_t index, solar_os_controller_info_t *info);
esp_err_t solar_os_controller_find(const char *name, solar_os_controller_info_t *info);
/* Numeric configuration. Only setpoint may change while an output is claimed. */
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
