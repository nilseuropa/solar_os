#include "solar_os_sensors.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "solar_os_stream.h"

static solar_os_sensors_provider_t sensors_provider;
static char sensors_provider_owner[24];
static bool sensors_initialized;
static bool temperature_stream_registered;
static bool humidity_stream_registered;
static portMUX_TYPE sensors_provider_lock = portMUX_INITIALIZER_UNLOCKED;

static bool sensors_provider_snapshot(solar_os_sensors_provider_t *provider)
{
    if (provider == NULL) {
        return false;
    }
    portENTER_CRITICAL(&sensors_provider_lock);
    const bool available = sensors_provider_owner[0] != '\0';
    if (available) {
        *provider = sensors_provider;
    }
    portEXIT_CRITICAL(&sensors_provider_lock);
    return available;
}

static esp_err_t sensors_stream_read_scalar(
    void *user,
    const solar_os_stream_read_options_t *options,
    float *value)
{
    (void)options;
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_environment_t environment;
    const esp_err_t err = solar_os_sensors_read_environment(&environment);
    if (err == ESP_OK) {
        *value = (uintptr_t)user == 0U ?
            environment.temperature_c : environment.humidity_percent;
    }
    return err;
}

static esp_err_t sensors_register_stream(const char *id,
                                         const char *unit,
                                         const char *summary,
                                         uintptr_t field)
{
    solar_os_stream_driver_t driver = {
        .info = {
            .type = SOLAR_OS_STREAM_TYPE_SCALAR,
            .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
            .sharing = SOLAR_OS_STREAM_SHARING_SHARED,
        },
        .read_scalar = sensors_stream_read_scalar,
        .user = (void *)field,
    };
    strlcpy(driver.info.id, id, sizeof(driver.info.id));
    strlcpy(driver.info.provider, "sensors", sizeof(driver.info.provider));
    strlcpy(driver.info.device, "environment0", sizeof(driver.info.device));
    strlcpy(driver.info.unit, unit, sizeof(driver.info.unit));
    strlcpy(driver.info.format, "f32", sizeof(driver.info.format));
    strlcpy(driver.info.summary, summary, sizeof(driver.info.summary));
    return solar_os_stream_register(&driver);
}

esp_err_t solar_os_sensors_init(void)
{
    sensors_initialized = true;
    solar_os_sensors_provider_t provider;
    if (!sensors_provider_snapshot(&provider)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (provider.has_temperature && !temperature_stream_registered) {
        esp_err_t err = sensors_register_stream("temperature", "C", "ambient temperature", 0U);
        if (err != ESP_OK) {
            return err;
        }
        temperature_stream_registered = true;
    }
    if (provider.has_humidity && !humidity_stream_registered) {
        esp_err_t err = sensors_register_stream("humidity", "percent", "relative humidity", 1U);
        if (err != ESP_OK) {
            if (temperature_stream_registered) {
                (void)solar_os_stream_unregister("temperature");
                temperature_stream_registered = false;
            }
            return err;
        }
        humidity_stream_registered = true;
    }
    return ESP_OK;
}

esp_err_t solar_os_sensors_register_provider(
    const char *owner,
    const solar_os_sensors_provider_t *provider)
{
    if (owner == NULL || owner[0] == '\0' ||
        strnlen(owner, sizeof(sensors_provider_owner)) >= sizeof(sensors_provider_owner) ||
        provider == NULL || provider->read_environment == NULL ||
        (!provider->has_temperature && !provider->has_humidity)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    portENTER_CRITICAL(&sensors_provider_lock);
    if (sensors_provider_owner[0] != '\0') {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        sensors_provider = *provider;
        strlcpy(sensors_provider_owner, owner, sizeof(sensors_provider_owner));
    }
    portEXIT_CRITICAL(&sensors_provider_lock);
    if (ret == ESP_OK && sensors_initialized) {
        ret = solar_os_sensors_init();
        if (ret != ESP_OK) {
            (void)solar_os_sensors_unregister_provider(owner);
        }
    }
    return ret;
}

esp_err_t solar_os_sensors_unregister_provider(const char *owner)
{
    if (owner == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&sensors_provider_lock);
    if (strcmp(sensors_provider_owner, owner) != 0) {
        portEXIT_CRITICAL(&sensors_provider_lock);
        return ESP_ERR_NOT_FOUND;
    }
    memset(&sensors_provider, 0, sizeof(sensors_provider));
    sensors_provider_owner[0] = '\0';
    portEXIT_CRITICAL(&sensors_provider_lock);

    if (temperature_stream_registered) {
        (void)solar_os_stream_unregister("temperature");
        temperature_stream_registered = false;
    }
    if (humidity_stream_registered) {
        (void)solar_os_stream_unregister("humidity");
        humidity_stream_registered = false;
    }
    return ESP_OK;
}

bool solar_os_sensors_has_provider(void)
{
    portENTER_CRITICAL(&sensors_provider_lock);
    const bool available = sensors_provider_owner[0] != '\0';
    portEXIT_CRITICAL(&sensors_provider_lock);
    return available;
}

esp_err_t solar_os_sensors_read_environment(solar_os_environment_t *environment)
{
    if (environment == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_sensors_provider_t provider;
    if (!sensors_provider_snapshot(&provider)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return provider.read_environment(provider.user, environment);
}
