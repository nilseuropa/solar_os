#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_sensors.h"
#include "solar_os_stream.h"

typedef struct {
    const char *name;
    float temperature_c;
    float humidity_percent;
    unsigned reads;
    bool try_unregister;
} fake_sensor_t;

static unsigned temperature_stream_registers;
static unsigned humidity_stream_registers;
static unsigned temperature_stream_unregisters;
static unsigned humidity_stream_unregisters;
static uint32_t temperature_stream_handles;
static uint32_t humidity_stream_handles;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t length = strlen(src);
    if (size > 0U) {
        const size_t copy = length < size - 1U ? length : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return length;
}

esp_err_t solar_os_stream_register(const solar_os_stream_driver_t *driver)
{
    assert(driver != NULL);
    assert(strcmp(driver->info.provider, "sensors") == 0);
    assert(strcmp(driver->info.device, "default") == 0);
    if (strcmp(driver->info.id, "temperature") == 0) {
        temperature_stream_registers++;
    } else {
        assert(strcmp(driver->info.id, "humidity") == 0);
        humidity_stream_registers++;
    }
    return ESP_OK;
}

esp_err_t solar_os_stream_unregister(const char *id)
{
    assert(id != NULL);
    if (strcmp(id, "temperature") == 0) {
        temperature_stream_unregisters++;
    } else {
        assert(strcmp(id, "humidity") == 0);
        humidity_stream_unregisters++;
    }
    return ESP_OK;
}

esp_err_t solar_os_stream_get_info(const char *id, solar_os_stream_info_t *info)
{
    assert(id != NULL);
    assert(info != NULL);
    memset(info, 0, sizeof(*info));
    if (strcmp(id, "temperature") == 0) {
        info->active_handles = temperature_stream_handles;
    } else {
        assert(strcmp(id, "humidity") == 0);
        info->active_handles = humidity_stream_handles;
    }
    return ESP_OK;
}

static esp_err_t fake_read(void *user, solar_os_environment_t *environment)
{
    fake_sensor_t *fake = user;
    assert(fake != NULL);
    assert(environment != NULL);
    fake->reads++;
    if (fake->try_unregister) {
        assert(solar_os_sensors_unregister(fake->name) == ESP_ERR_INVALID_STATE);
    }
    *environment = (solar_os_environment_t) {
        .temperature_c = fake->temperature_c,
        .humidity_percent = fake->humidity_percent,
    };
    return ESP_OK;
}

static esp_err_t fake_read_temperature(void *user, float *temperature_c)
{
    fake_sensor_t *fake = user;
    assert(fake != NULL);
    assert(temperature_c != NULL);
    fake->reads++;
    *temperature_c = fake->temperature_c;
    return ESP_OK;
}

static esp_err_t fake_read_humidity(void *user, float *humidity_percent)
{
    fake_sensor_t *fake = user;
    assert(fake != NULL);
    assert(humidity_percent != NULL);
    fake->reads++;
    *humidity_percent = fake->humidity_percent;
    return ESP_OK;
}

static void register_fake(fake_sensor_t *fake,
                          const char *driver,
                          bool temperature,
                          bool humidity)
{
    const solar_os_sensors_ops_t ops = {
        .read_environment = temperature && humidity ? fake_read : NULL,
        .read_temperature = temperature && !humidity ? fake_read_temperature : NULL,
        .read_humidity = humidity && !temperature ? fake_read_humidity : NULL,
    };
    const solar_os_sensors_registration_t registration = {
        .name = fake->name,
        .driver = driver,
        .ops = &ops,
        .ctx = fake,
    };
    assert(solar_os_sensors_register(&registration) == ESP_OK);
}

int main(void)
{
    assert(solar_os_sensors_init() == ESP_OK);
    assert(temperature_stream_registers == 0U);
    assert(humidity_stream_registers == 0U);

    fake_sensor_t combined = {
        .name = "environment0",
        .temperature_c = 21.5f,
        .humidity_percent = 40.0f,
        .try_unregister = true,
    };
    fake_sensor_t temperature = {
        .name = "temperature1",
        .temperature_c = -5.25f,
    };
    fake_sensor_t humidity = {
        .name = "humidity1",
        .humidity_percent = 82.5f,
    };
    fake_sensor_t split_environment = {
        .name = "environment2",
        .temperature_c = 12.25f,
        .humidity_percent = 63.5f,
    };

    register_fake(&combined, "combined-fake", true, true);
    register_fake(&temperature, "temperature-fake", true, false);
    register_fake(&humidity, "humidity-fake", false, true);
    const solar_os_sensors_ops_t split_ops = {
        .read_temperature = fake_read_temperature,
        .read_humidity = fake_read_humidity,
    };
    const solar_os_sensors_registration_t split_registration = {
        .name = split_environment.name,
        .driver = "split-fake",
        .ops = &split_ops,
        .ctx = &split_environment,
    };
    assert(solar_os_sensors_register(&split_registration) == ESP_OK);
    assert(solar_os_sensors_count() == 4U);

    solar_os_sensor_info_t info;
    assert(solar_os_sensors_get(0U, &info));
    assert(strcmp(info.name, combined.name) == 0);
    assert(strcmp(info.driver, "combined-fake") == 0);
    assert(info.capabilities ==
           (SOLAR_OS_SENSOR_CAP_TEMPERATURE | SOLAR_OS_SENSOR_CAP_HUMIDITY));
    assert(solar_os_sensors_get(2U, &info));
    assert(strcmp(info.name, humidity.name) == 0);
    assert(solar_os_sensors_get(3U, &info));
    assert(strcmp(info.name, split_environment.name) == 0);
    assert(!solar_os_sensors_get(4U, &info));

    assert(solar_os_sensors_init() == ESP_OK);
    assert(temperature_stream_registers == 1U);
    assert(humidity_stream_registers == 1U);

    solar_os_environment_t environment;
    assert(solar_os_sensors_read_environment(&environment) == ESP_OK);
    assert(environment.temperature_c == combined.temperature_c);
    assert(environment.humidity_percent == combined.humidity_percent);
    assert(combined.reads == 1U);

    float value = 0.0f;
    assert(solar_os_sensors_read_temperature("", &value) == ESP_ERR_INVALID_ARG);
    assert(solar_os_sensors_read_temperature(temperature.name, &value) == ESP_OK);
    assert(value == temperature.temperature_c);
    assert(solar_os_sensors_read_humidity(humidity.name, &value) == ESP_OK);
    assert(value == humidity.humidity_percent);
    assert(solar_os_sensors_read_environment_from(temperature.name, &environment) ==
           ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_sensors_read_environment_from(
               split_environment.name, &environment) == ESP_OK);
    assert(environment.temperature_c == split_environment.temperature_c);
    assert(environment.humidity_percent == split_environment.humidity_percent);
    assert(split_environment.reads == 2U);

    assert(solar_os_sensors_unregister(combined.name) == ESP_OK);
    assert(solar_os_sensors_unregister(split_environment.name) == ESP_OK);
    assert(temperature_stream_unregisters == 0U);
    assert(humidity_stream_unregisters == 0U);
    assert(solar_os_sensors_read_environment(&environment) == ESP_OK);
    assert(environment.temperature_c == temperature.temperature_c);
    assert(environment.humidity_percent == humidity.humidity_percent);

    temperature_stream_handles = 1U;
    assert(solar_os_sensors_unregister(temperature.name) == ESP_ERR_INVALID_STATE);
    assert(solar_os_sensors_count() == 2U);
    temperature_stream_handles = 0U;
    assert(solar_os_sensors_unregister(temperature.name) == ESP_OK);
    assert(temperature_stream_unregisters == 1U);
    assert(humidity_stream_unregisters == 0U);
    assert(solar_os_sensors_read_temperature(NULL, &value) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_sensors_read_humidity(NULL, &value) == ESP_OK);

    assert(solar_os_sensors_unregister(humidity.name) == ESP_OK);
    assert(humidity_stream_unregisters == 1U);
    assert(!solar_os_sensors_has_provider());

    puts("sensor service registry tests: ok");
    return 0;
}
