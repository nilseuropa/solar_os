#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    float temperature_c;
    float humidity_percent;
} solar_os_environment_t;

typedef esp_err_t (*solar_os_sensors_provider_read_fn_t)(
    void *user,
    solar_os_environment_t *environment);

typedef struct {
    solar_os_sensors_provider_read_fn_t read_environment;
    void *user;
    bool has_temperature;
    bool has_humidity;
} solar_os_sensors_provider_t;

esp_err_t solar_os_sensors_init(void);
esp_err_t solar_os_sensors_register_provider(
    const char *owner,
    const solar_os_sensors_provider_t *provider);
esp_err_t solar_os_sensors_unregister_provider(const char *owner);
bool solar_os_sensors_has_provider(void);
esp_err_t solar_os_sensors_read_environment(solar_os_environment_t *environment);
