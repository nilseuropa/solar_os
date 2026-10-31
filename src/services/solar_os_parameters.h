#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_PARAMETER_OWNER_MAX 24U
#define SOLAR_OS_PARAMETER_NAME_MAX 32U
#define SOLAR_OS_PARAMETER_PATH_MAX 64U
#define SOLAR_OS_PARAMETER_LABEL_MAX 40U
#define SOLAR_OS_PARAMETER_UNIT_MAX 12U
#define SOLAR_OS_PARAMETER_MAX 48U
#define SOLAR_OS_PARAMETER_NORMALIZED_MAX UINT16_MAX

typedef enum {
    SOLAR_OS_PARAMETER_CURVE_LINEAR = 0,
    SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC,
} solar_os_parameter_curve_t;

typedef esp_err_t (*solar_os_parameter_get_fn)(void *user, float *value);
typedef esp_err_t (*solar_os_parameter_set_fn)(void *user, float value);

typedef struct {
    const char *name;
    const char *label;
    const char *unit;
    float minimum;
    float maximum;
    float step;
    solar_os_parameter_curve_t curve;
    solar_os_parameter_get_fn get;
    solar_os_parameter_set_fn set;
    void *user;
} solar_os_parameter_definition_t;

typedef struct {
    size_t count;
    uint32_t token;
} solar_os_parameter_registration_t;

#define SOLAR_OS_PARAMETER_REGISTRATION_INIT \
    { .count = 0U, .token = 0U }

typedef struct {
    char path[SOLAR_OS_PARAMETER_PATH_MAX];
    char owner[SOLAR_OS_PARAMETER_OWNER_MAX];
    char name[SOLAR_OS_PARAMETER_NAME_MAX];
    char label[SOLAR_OS_PARAMETER_LABEL_MAX];
    char unit[SOLAR_OS_PARAMETER_UNIT_MAX];
    float minimum;
    float maximum;
    float step;
    solar_os_parameter_curve_t curve;
} solar_os_parameter_info_t;

esp_err_t solar_os_parameters_register(
    const char *owner,
    const solar_os_parameter_definition_t *definitions,
    size_t count,
    solar_os_parameter_registration_t *registration);
/* Retires the registration immediately. ESP_ERR_INVALID_STATE means callbacks
 * remain in flight: retain their state and retry until ESP_OK. Retiring names
 * cannot be reused. A callback must not synchronously wait for its own removal. */
esp_err_t solar_os_parameters_unregister(
    solar_os_parameter_registration_t *registration);

size_t solar_os_parameter_count(void);
bool solar_os_parameter_get_info(size_t index, solar_os_parameter_info_t *info);
esp_err_t solar_os_parameter_find(const char *path,
                                  solar_os_parameter_info_t *info);
esp_err_t solar_os_parameter_get(const char *path, float *value);
esp_err_t solar_os_parameter_set(const char *path, float value);
esp_err_t solar_os_parameter_get_normalized(const char *path, uint16_t *value);
esp_err_t solar_os_parameter_set_normalized(const char *path, uint16_t value);

const char *solar_os_parameter_curve_name(solar_os_parameter_curve_t curve);
