#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_PWM_FREQ_MIN_HZ 1U
#define SOLAR_OS_PWM_FREQ_MAX_HZ 40000U
#define SOLAR_OS_PWM_DUTY_MAX_PERCENT 100U
#define SOLAR_OS_PWM_PROVIDER_MAX 4U
#define SOLAR_OS_PWM_OUTPUT_MAX 32U
#define SOLAR_OS_PWM_NAME_MAX 20U
#define SOLAR_OS_PWM_DRIVER_MAX 24U
#define SOLAR_OS_PWM_OWNER_MAX 24U
#define SOLAR_OS_PWM_ENDPOINT_MAX 27U

typedef struct {
    char provider[SOLAR_OS_PWM_NAME_MAX];
    uint16_t output;
} solar_os_pwm_output_t;

typedef struct {
    char provider[SOLAR_OS_PWM_NAME_MAX];
    char owner[SOLAR_OS_PWM_OWNER_MAX];
    uint16_t output;
    uint16_t frequency_group;
    int pin;
    bool runtime_allowed;
    bool active;
    int channel;
    uint32_t freq_hz;
    uint8_t duty_percent;
    bool enabled;
    bool reserved;
    uint8_t resolution_bits;
    uint32_t pulse_width_us;
} solar_os_pwm_output_info_t;
typedef solar_os_pwm_output_info_t solar_os_pwm_pin_info_t;

/* get_output enumerates the provider's usable logical outputs. Native output
 * IDs are GPIO numbers; expansion output IDs are controller channels. Backend
 * callbacks run serialized per provider, outside the global registry lock.
 * They must not re-enter this provider through the PWM service. */
typedef struct {
    size_t (*output_count)(void *ctx);
    bool (*get_output)(void *ctx, size_t index, solar_os_pwm_output_info_t *info);
    esp_err_t (*set)(void *ctx, uint16_t output, uint32_t freq_hz, uint32_t value, bool pulse);
    esp_err_t (*open)(void *ctx, uint16_t output, uint32_t freq_hz, const char *owner);
    esp_err_t (*write_pulse)(void *ctx, uint16_t output, const char *owner, uint32_t pulse_us);
    esp_err_t (*disable)(void *ctx, uint16_t output, const char *owner);
    esp_err_t (*close)(void *ctx, uint16_t output, const char *owner);
    esp_err_t (*stop)(void *ctx, uint16_t output);
} solar_os_pwm_ops_t;

typedef struct {
    const char *name;
    const char *driver;
    bool detachable;
    const solar_os_pwm_ops_t *ops;
    void *ctx;
} solar_os_pwm_registration_t;

typedef struct {
    char name[SOLAR_OS_PWM_NAME_MAX];
    char driver[SOLAR_OS_PWM_DRIVER_MAX];
    bool detachable;
    size_t output_count;
} solar_os_pwm_provider_info_t;

/* Attach registers a provider; unregister fails while any output is reserved,
 * active, or in flight. The driver retains its context until unregister succeeds. */
esp_err_t solar_os_pwm_register(const solar_os_pwm_registration_t *registration);
esp_err_t solar_os_pwm_unregister(const char *name);
esp_err_t solar_os_pwm_unregister_owned(const char *name, const void *ctx);
size_t solar_os_pwm_provider_count(void);
bool solar_os_pwm_get_provider(size_t index, solar_os_pwm_provider_info_t *info);
bool solar_os_pwm_parse_output(const char *text, solar_os_pwm_output_t *output);
void solar_os_pwm_format_output(const solar_os_pwm_output_t *output, char *text, size_t size);
size_t solar_os_pwm_output_count(void);
bool solar_os_pwm_get_output_info(size_t index, solar_os_pwm_output_info_t *info);
esp_err_t solar_os_pwm_set_output(const solar_os_pwm_output_t *output, uint32_t freq_hz,
                                  uint32_t value, bool pulse);
esp_err_t solar_os_pwm_open_output(const solar_os_pwm_output_t *output, uint32_t freq_hz,
                                   const char *owner);
esp_err_t solar_os_pwm_write_output(const solar_os_pwm_output_t *output, const char *owner,
                                    uint32_t pulse_us);
esp_err_t solar_os_pwm_disable_output(const solar_os_pwm_output_t *output, const char *owner);
esp_err_t solar_os_pwm_close_output(const solar_os_pwm_output_t *output, const char *owner);
esp_err_t solar_os_pwm_stop_output(const solar_os_pwm_output_t *output);

esp_err_t solar_os_pwm_init(void);
size_t solar_os_pwm_pin_count(void);
bool solar_os_pwm_get_pin_info(size_t index, solar_os_pwm_pin_info_t *info);
esp_err_t solar_os_pwm_set(int pin, uint32_t freq_hz, uint8_t duty_percent);
esp_err_t solar_os_pwm_set_pulse(int pin, uint32_t freq_hz, uint32_t pulse_us);
/* Legacy GPIO helpers address native:<pin>. */
esp_err_t solar_os_pwm_open(int pin, uint32_t freq_hz, const char *owner);
esp_err_t solar_os_pwm_write_pulse(int pin, const char *owner, uint32_t pulse_us);
esp_err_t solar_os_pwm_disable(int pin, const char *owner);
esp_err_t solar_os_pwm_close(int pin, const char *owner);
esp_err_t solar_os_pwm_stop(int pin);
