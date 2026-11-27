#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_EXPANSION_DRIVER_NAME_MAX 32
#define SOLAR_OS_EXPANSION_DEVICE_NAME_MAX 20
#define SOLAR_OS_EXPANSION_ROLE_MAX 16
#define SOLAR_OS_EXPANSION_TARGET_MAX 16
#define SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX 24

typedef enum {
    SOLAR_OS_EXPANSION_BINDING_GPIO,
    SOLAR_OS_EXPANSION_BINDING_GPIO_LINE,
    SOLAR_OS_EXPANSION_BINDING_ADC,
    SOLAR_OS_EXPANSION_BINDING_PWM,
    SOLAR_OS_EXPANSION_BINDING_I2S_PORT,
    SOLAR_OS_EXPANSION_BINDING_I2C_BUS,
    SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS,
    SOLAR_OS_EXPANSION_BINDING_SPI_BUS,
    SOLAR_OS_EXPANSION_BINDING_SPI_CS,
    SOLAR_OS_EXPANSION_BINDING_UART_PORT,
    SOLAR_OS_EXPANSION_BINDING_PS2_BUS,
    SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM,
    SOLAR_OS_EXPANSION_BINDING_PARAMETER,
    SOLAR_OS_EXPANSION_BINDING_BYTE_STREAM,
    SOLAR_OS_EXPANSION_BINDING_DISPLAY_TARGET,
} solar_os_expansion_binding_kind_t;

typedef struct {
    solar_os_expansion_binding_kind_t kind;
    char role[SOLAR_OS_EXPANSION_ROLE_MAX];
    char target[SOLAR_OS_EXPANSION_TARGET_MAX];
    int value;
    int aux;
} solar_os_expansion_binding_t;

typedef enum {
    SOLAR_OS_EXPANSION_BINDINGS_VALID,
    SOLAR_OS_EXPANSION_BINDINGS_MISSING,
    SOLAR_OS_EXPANSION_BINDINGS_UNEXPECTED,
    SOLAR_OS_EXPANSION_BINDINGS_DUPLICATE,
    SOLAR_OS_EXPANSION_BINDINGS_INVALID_VALUE,
    SOLAR_OS_EXPANSION_BINDINGS_UNAVAILABLE,
} solar_os_expansion_binding_validation_reason_t;

typedef struct {
    solar_os_expansion_binding_validation_reason_t reason;
    char key[SOLAR_OS_EXPANSION_ROLE_MAX];
} solar_os_expansion_binding_validation_t;

typedef enum {
    SOLAR_OS_EXPANSION_CATEGORY_AUDIO,
    SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    SOLAR_OS_EXPANSION_CATEGORY_INPUT,
    SOLAR_OS_EXPANSION_CATEGORY_POWER,
    SOLAR_OS_EXPANSION_CATEGORY_RADIO,
    SOLAR_OS_EXPANSION_CATEGORY_SENSOR,
    SOLAR_OS_EXPANSION_CATEGORY_STORAGE,
    SOLAR_OS_EXPANSION_CATEGORY_UTILITY,
    SOLAR_OS_EXPANSION_CATEGORY_COUNT,
} solar_os_expansion_category_t;

typedef enum {
    SOLAR_OS_EXPANSION_ORIGIN_BOARD,
    SOLAR_OS_EXPANSION_ORIGIN_RUNTIME,
} solar_os_expansion_origin_t;

typedef struct {
    bool active;
    bool ready;
    bool autostart;
    bool detachable;
    solar_os_expansion_origin_t origin;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char driver[SOLAR_OS_EXPANSION_DRIVER_NAME_MAX];
    size_t binding_count;
    solar_os_expansion_binding_t bindings[SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX];
} solar_os_expansion_device_t;

#define SOLAR_OS_EXPANSION_DRIVER_SUMMARY_MAX 96U
#define SOLAR_OS_EXPANSION_BINDING_HINT_MAX 64U

typedef struct {
    char name[SOLAR_OS_EXPANSION_DRIVER_NAME_MAX];
    char summary[SOLAR_OS_EXPANSION_DRIVER_SUMMARY_MAX];
    uint64_t required_capabilities;
    solar_os_expansion_category_t category;
    size_t binding_spec_count;
    bool probe_supported, allow_unlisted_bindings;
} solar_os_expansion_driver_info_t;

/* A resident driver may borrow immutable schema strings and allowed values. */
typedef struct {
    const char *key;
    const char *value_hint;
    solar_os_expansion_binding_kind_t kind;
    const char *role;
    bool required;
    const int *allowed_values;
    size_t allowed_value_count;
    bool has_value_range;
    int min_value;
    int max_value;
} solar_os_expansion_binding_spec_t;

typedef struct {
    char key[SOLAR_OS_EXPANSION_ROLE_MAX], role[SOLAR_OS_EXPANSION_ROLE_MAX];
    char value_hint[SOLAR_OS_EXPANSION_BINDING_HINT_MAX];
    solar_os_expansion_binding_kind_t kind;
    size_t allowed_value_count;
    int min_value, max_value;
    bool required, has_role, has_value_hint, has_value_range;
} solar_os_expansion_binding_spec_info_t;
