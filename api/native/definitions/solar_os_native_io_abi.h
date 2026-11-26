#pragma once
#include "solar_os_native_service_abi.h"
#define SOLAR_OS_NATIVE_RESOURCES_SERVICE "resources"
#define SOLAR_OS_NATIVE_RESOURCES_ABI 1U
#define SOLAR_OS_NATIVE_PINS_SERVICE "pins"
#define SOLAR_OS_NATIVE_PINS_ABI 1U
#define SOLAR_OS_NATIVE_GPIO_SERVICE "gpio"
#define SOLAR_OS_NATIVE_GPIO_ABI 1U
#define SOLAR_OS_NATIVE_ADC_SERVICE "adc"
#define SOLAR_OS_NATIVE_ADC_ABI 1U
#define SOLAR_OS_NATIVE_PWM_SERVICE "pwm"
#define SOLAR_OS_NATIVE_PWM_ABI 1U
#define SOLAR_OS_NATIVE_RESOURCE_GPIO_PIN 0U
#define SOLAR_OS_NATIVE_RESOURCE_GPIO_LINE 1U
#define SOLAR_OS_NATIVE_RESOURCE_ADC_PIN 2U
#define SOLAR_OS_NATIVE_RESOURCE_PWM_PIN 3U
#define SOLAR_OS_NATIVE_RESOURCE_I2C_PORT 4U
#define SOLAR_OS_NATIVE_RESOURCE_SPI_HOST 5U
#define SOLAR_OS_NATIVE_RESOURCE_I2C_ADDRESS 6U
#define SOLAR_OS_NATIVE_RESOURCE_SPI_CS 7U
#define SOLAR_OS_NATIVE_RESOURCE_UART_PORT 8U
#define SOLAR_OS_NATIVE_RESOURCE_I2S_PORT 9U
#define SOLAR_OS_NATIVE_RESOURCE_CAMERA_PORT 10U
#define SOLAR_OS_NATIVE_RESOURCE_USB_PORT 11U
#define SOLAR_OS_NATIVE_PIN_FIXED 0U
#define SOLAR_OS_NATIVE_PIN_RELEASABLE 1U
#define SOLAR_OS_NATIVE_PIN_FREE 2U
#define SOLAR_OS_NATIVE_PIN_GPIO 0U
#define SOLAR_OS_NATIVE_PIN_POWER 1U
#define SOLAR_OS_NATIVE_PIN_GROUND 2U
#define SOLAR_OS_NATIVE_PIN_CONTROL 3U
#define SOLAR_OS_NATIVE_PIN_NC 4U
#define SOLAR_OS_NATIVE_GPIO_INPUT 0U
#define SOLAR_OS_NATIVE_GPIO_OUTPUT 1U
#define SOLAR_OS_NATIVE_GPIO_PULL_NONE 0U
#define SOLAR_OS_NATIVE_GPIO_PULL_UP 1U
#define SOLAR_OS_NATIVE_GPIO_PULL_DOWN 2U
#define SOLAR_OS_NATIVE_GPIO_LABEL_MODE 0U
#define SOLAR_OS_NATIVE_GPIO_LABEL_PULL 1U
typedef struct { uint32_t kind; int32_t primary, secondary; const char *label; } solar_os_native_resource_request_v1_t;
typedef struct { uint32_t active, kind; int32_t primary, secondary; char owner[24], label[24]; } solar_os_native_resource_claim_v1_t;
typedef struct { uint64_t index; solar_os_native_resource_claim_v1_t existing; } solar_os_native_resource_conflict_v1_t;
typedef struct { int32_t pin; uint32_t expansion, direct_gpio, policy; char role[128]; } solar_os_native_pin_info_v1_t;
typedef struct { uint64_t rows, columns; char title[128], view[64]; } solar_os_native_connector_layout_v1_t;
typedef struct { int32_t pin; uint32_t position, row, column, kind; char connector[32], label[64]; } solar_os_native_connector_pin_v1_t;
typedef struct {
    int32_t pin;
    uint32_t expansion, runtime_allowed, available, claimed, policy, configured, mode, pull, level, level_valid;
    char owner[24], role[128];
} solar_os_native_gpio_info_v1_t;
typedef struct { int32_t pin, unit, channel; uint32_t runtime_allowed, adc_capable; } solar_os_native_adc_info_v1_t;
typedef struct { int32_t pin, raw, unit, channel; uint32_t voltage_mv, calibrated; } solar_os_native_adc_sample_v1_t;
typedef struct { char provider[20]; uint32_t output; } solar_os_native_pwm_output_v1_t;
typedef struct { char name[20], driver[24]; uint64_t output_count; uint32_t detachable; } solar_os_native_pwm_provider_v1_t;
typedef struct {
    char provider[20], owner[24];
    uint32_t output, frequency_group, runtime_allowed, active, frequency_hz, duty_percent, enabled, reserved, resolution_bits, pulse_width_us;
    int32_t pin, channel;
} solar_os_native_pwm_info_v1_t;
/* Claims reserve registry capacity before acquiring OS resources. Their owner
 * key contains a unique token prefix, so equal caller labels never alias.
 * Closing retries unfinished cleanup; successfully released members of a
 * bundle are not released again. No caller strings survive acquisition.
 * Metadata is copied; insufficient fixed text capacity returns INVALID_SIZE.
 * These services enforce existing OS pin/resource/provider policy. */
typedef struct {
    uint32_t abi_version, struct_size;
    size_t (*count)(void);
    int (*info)(size_t index, solar_os_native_resource_claim_v1_t *claim);
    int (*find)(uint32_t kind, int32_t primary, int32_t secondary, solar_os_native_resource_claim_v1_t *claim);
    int (*claim)(const solar_os_native_resource_request_v1_t *requests, size_t count, const char *owner,
        solar_os_native_resource_conflict_v1_t *conflict, solar_os_native_handle_t *lease);
    int (*close)(solar_os_native_handle_t lease);
    const char *(*kind_name)(uint32_t kind);
} solar_os_native_resources_api_v1_t;
typedef struct {
    uint32_t abi_version, struct_size;
    size_t (*count)(void);
    int (*info)(size_t index, solar_os_native_pin_info_v1_t *info);
    int (*find)(int32_t pin, solar_os_native_pin_info_v1_t *info);
    bool (*is_expansion)(int32_t pin);
    bool (*is_direct_gpio)(int32_t pin);
    bool (*is_routable)(int32_t pin);
    int (*connector_layout)(solar_os_native_connector_layout_v1_t *layout);
    size_t (*connector_count)(void);
    int (*connector_info)(size_t index, solar_os_native_connector_pin_v1_t *info);
    int (*connector_find)(size_t row, size_t column, solar_os_native_connector_pin_v1_t *info, size_t *index);
    bool (*connector_exists)(const char *name);
    const char *(*policy_name)(uint32_t policy);
    const char *(*kind_name)(uint32_t kind);
} solar_os_native_pins_api_v1_t;
#define SOLAR_OS_GPIO_CONTROLLER_NAME_MAX 16U

typedef struct {
    char controller[SOLAR_OS_GPIO_CONTROLLER_NAME_MAX]; /* Empty for an ESP GPIO. */
    uint8_t line;
} solar_os_native_gpio_line_v1_t;

typedef struct {
    uint8_t id, line_count;
    char name[SOLAR_OS_GPIO_CONTROLLER_NAME_MAX];
} solar_os_native_gpio_controller_v1_t;

typedef struct {
    const char *name;
    uint8_t line_count;
    uint32_t library_lease;
    void *user;
    int (*configure)(void *user, uint8_t line, uint32_t mode, uint32_t pull);
    int (*read)(void *user, uint8_t line, bool *level);
    int (*write)(void *user, uint8_t line, bool level);
    int (*acknowledge_interrupt)(void *user);
} solar_os_native_gpio_controller_provider_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    size_t (*count)(void);
    int (*info)(size_t index, solar_os_native_gpio_info_v1_t *info);
    int (*find)(int32_t pin, solar_os_native_gpio_info_v1_t *info);
    bool (*runtime_allowed)(int32_t pin);
    int (*configure)(int32_t pin, uint32_t mode, uint32_t pull);
    int (*read)(int32_t pin, bool *level);
    int (*write)(int32_t pin, bool level);
    int (*release)(int32_t pin);
    int (*claim)(const int32_t *pins, const char *const *labels, size_t count, const char *owner,
        solar_os_native_resource_conflict_v1_t *conflict, solar_os_native_handle_t *lease);
    int (*configure_owned)(solar_os_native_handle_t lease, int32_t pin, uint32_t mode, uint32_t pull);
    int (*read_owned)(solar_os_native_handle_t lease, int32_t pin, bool *level);
    int (*write_owned)(solar_os_native_handle_t lease, int32_t pin, bool level);
    int (*close)(solar_os_native_handle_t lease);
    /* Shared shell names and aliases for mode and pull values. */
    const char *(*label)(uint32_t field, uint32_t value);
    bool (*parse)(uint32_t field, const char *text, uint32_t *value);
    /* Available with expansion support. Numeric GPIO and controller:line
     * use the same syntax as Core device bindings. IDs may be reused. */
    bool (*line_parse)(const char *text, solar_os_native_gpio_line_v1_t *line);
    bool (*line_is_native)(const solar_os_native_gpio_line_v1_t *line);
    size_t (*controller_count)(void);
    bool (*controller_info)(size_t index, solar_os_native_gpio_controller_v1_t *info);
    bool (*controller_find)(const char *name, solar_os_native_gpio_controller_v1_t *info);
    /* Pin the named controller while the lease is open. Numeric lines use
     * the runtime GPIO pin rules and must be configured before reading or
     * writing. A failed close retains the lease for retry. */
    int (*line_open)(const solar_os_native_gpio_line_v1_t *line, const char *owner,
        solar_os_native_handle_t *lease);
    int (*line_configure)(solar_os_native_handle_t lease, uint32_t mode, uint32_t pull);
    int (*line_read)(solar_os_native_handle_t lease, bool *level);
    int (*line_write)(solar_os_native_handle_t lease, bool level);
    /* Acknowledges the expander interrupt shared by its input lines. */
    int (*line_acknowledge_interrupt)(solar_os_native_handle_t lease);
    int (*line_close)(solar_os_native_handle_t lease);
    /* Copy callbacks and retain the code lease until closed. A claimed line
     * or running callback keeps the provider registered. Retry close then. */
    int (*controller_register)(const solar_os_native_gpio_controller_provider_v1_t *provider,
        solar_os_native_handle_t *handle);
    int (*controller_close)(solar_os_native_handle_t handle);
} solar_os_native_gpio_api_v1_t;
typedef struct {
    uint32_t abi_version, struct_size;
    size_t (*count)(void);
    int (*info)(size_t index, solar_os_native_adc_info_v1_t *info);
    int (*read)(int32_t pin, solar_os_native_adc_sample_v1_t *sample);
} solar_os_native_adc_api_v1_t;
typedef struct {
    uint32_t abi_version, struct_size;
    size_t (*provider_count)(void);
    int (*provider_info)(size_t index, solar_os_native_pwm_provider_v1_t *info);
    size_t (*count)(void);
    int (*info)(size_t index, solar_os_native_pwm_info_v1_t *info);
    int (*parse)(const char *text, solar_os_native_pwm_output_v1_t *output);
    int (*format)(const solar_os_native_pwm_output_v1_t *output, char *text, size_t capacity);
    int (*set)(const solar_os_native_pwm_output_v1_t *output, uint32_t frequency_hz, uint32_t value, bool pulse);
    int (*stop)(const solar_os_native_pwm_output_v1_t *output);
    int (*open)(const solar_os_native_pwm_output_v1_t *output, uint32_t frequency_hz, const char *owner, solar_os_native_handle_t *lease);
    int (*write)(solar_os_native_handle_t lease, uint32_t pulse_width_us);
    int (*disable)(solar_os_native_handle_t lease);
    int (*close)(solar_os_native_handle_t lease);
} solar_os_native_pwm_api_v1_t;
