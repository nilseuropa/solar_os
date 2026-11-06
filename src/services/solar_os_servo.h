#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "solar_os_pwm.h"

#define SOLAR_OS_SERVO_MAX 16U
#define SOLAR_OS_SERVO_NAME_MAX 16U
#define SOLAR_OS_SERVO_OWNER_MAX 24U
#define SOLAR_OS_SERVO_FREQ_DEFAULT_HZ 50U
#define SOLAR_OS_SERVO_FREQ_MAX_HZ 1000U

typedef struct {
    solar_os_pwm_output_t output;
    uint32_t freq_hz;
    uint32_t pulse_min_us;
    uint32_t pulse_max_us;
    float angle_min_rad;
    float angle_max_rad;
    bool inverted;
} solar_os_servo_config_t;

typedef struct {
    char name[SOLAR_OS_SERVO_NAME_MAX];
    char owner[SOLAR_OS_SERVO_OWNER_MAX];
    solar_os_servo_config_t config;
    bool enabled;
    bool has_command;
    uint32_t commanded_pulse_us;
    float commanded_angle_rad;
    bool input_claimed, stream_writable, closing;
} solar_os_servo_info_t;

/* All angles, including calibration endpoints and readback, are radians.
 * Opening reserves hardware but sends no servo pulses. Calibration is explicit.
 * Set/disable/close require the same owner that opened the servo. */
esp_err_t solar_os_servo_open(const char *name, const char *owner,
                              const solar_os_servo_config_t *config);
/* Component-owned servo: expose command observation, retain owner-only input. */
esp_err_t solar_os_servo_open_private(const char *name, const char *owner,
                                      const solar_os_servo_config_t *config);
size_t solar_os_servo_count(void);
bool solar_os_servo_get(size_t index, solar_os_servo_info_t *info);
esp_err_t solar_os_servo_status(const char *name, solar_os_servo_info_t *info);
esp_err_t solar_os_servo_set(const char *name, const char *owner, float angle_rad);
esp_err_t solar_os_servo_pulse(const char *name, const char *owner, uint32_t pulse_us);
esp_err_t solar_os_servo_disable(const char *name, const char *owner);
esp_err_t solar_os_servo_close(const char *name, const char *owner);
/* Returns the first error; failed closes retain ownership for retry. */
esp_err_t solar_os_servo_release_owner(const char *owner);
