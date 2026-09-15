#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_SENSOR_NAME_MAX 20
#define SOLAR_OS_SENSOR_DRIVER_MAX 24

typedef uint32_t solar_os_sensor_capabilities_t;

typedef enum {
    SOLAR_OS_SENSOR_CAP_TEMPERATURE = 1U << 0,
    SOLAR_OS_SENSOR_CAP_HUMIDITY = 1U << 1,
} solar_os_sensor_capability_t;

typedef struct {
    float temperature_c;
    float humidity_percent;
} solar_os_environment_t;

typedef esp_err_t (*solar_os_sensors_provider_read_fn_t)(
    void *user,
    solar_os_environment_t *environment);
typedef esp_err_t (*solar_os_sensors_provider_read_temperature_fn_t)(
    void *user,
    float *temperature_c);
typedef esp_err_t (*solar_os_sensors_provider_read_humidity_fn_t)(
    void *user,
    float *humidity_percent);

typedef struct {
    solar_os_sensors_provider_read_fn_t read_environment;
    solar_os_sensors_provider_read_temperature_fn_t read_temperature;
    solar_os_sensors_provider_read_humidity_fn_t read_humidity;
} solar_os_sensors_ops_t;

typedef struct {
    const char *name;
    const char *driver;
    const solar_os_sensors_ops_t *ops;
    void *ctx;
} solar_os_sensors_registration_t;

typedef struct {
    char name[SOLAR_OS_SENSOR_NAME_MAX];
    char driver[SOLAR_OS_SENSOR_DRIVER_MAX];
    solar_os_sensor_capabilities_t capabilities;
} solar_os_sensor_info_t;

esp_err_t solar_os_sensors_init(void);
esp_err_t solar_os_sensors_register(
    const solar_os_sensors_registration_t *registration);
esp_err_t solar_os_sensors_unregister(const char *name);
size_t solar_os_sensors_count(void);
bool solar_os_sensors_get(size_t index, solar_os_sensor_info_t *info);
bool solar_os_sensors_has_provider(void);
esp_err_t solar_os_sensors_read_environment(solar_os_environment_t *environment);
esp_err_t solar_os_sensors_read_environment_from(
    const char *name,
    solar_os_environment_t *environment);
esp_err_t solar_os_sensors_read_temperature(const char *name, float *temperature_c);
esp_err_t solar_os_sensors_read_humidity(const char *name, float *humidity_percent);
