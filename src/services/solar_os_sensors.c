#include "solar_os_sensors.h"

#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_stream.h"

#define SENSOR_DEVICE_MAX 8U
#define SENSOR_CAP_ENVIRONMENT \
    (SOLAR_OS_SENSOR_CAP_TEMPERATURE | SOLAR_OS_SENSOR_CAP_HUMIDITY)

typedef struct {
    bool active;
    bool detaching;
    solar_os_sensor_info_t info;
    solar_os_sensors_ops_t ops;
    void *ctx;
    size_t refs;
    uint32_t generation;
} sensor_device_t;

typedef struct {
    size_t index;
    uint32_t generation;
    solar_os_sensors_ops_t ops;
    void *ctx;
} sensor_ref_t;

static EXT_RAM_BSS_ATTR sensor_device_t sensor_devices[SENSOR_DEVICE_MAX];
static SemaphoreHandle_t sensors_mutex;
static EXT_RAM_BSS_ATTR StaticSemaphore_t sensors_mutex_storage;
static uint32_t sensors_next_generation = 1U;
static bool sensors_initialized;
static bool temperature_stream_registered;
static bool humidity_stream_registered;

static esp_err_t ensure_mutex(void)
{
    if (sensors_mutex == NULL) {
        sensors_mutex = xSemaphoreCreateMutexStatic(&sensors_mutex_storage);
    }
    return sensors_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool sensor_name_valid(const char *name)
{
    return name != NULL && name[0] != '\0' &&
        strnlen(name, SOLAR_OS_SENSOR_NAME_MAX) < SOLAR_OS_SENSOR_NAME_MAX;
}

static solar_os_sensor_capabilities_t provider_capabilities(
    const solar_os_sensors_ops_t *ops)
{
    solar_os_sensor_capabilities_t capabilities = 0U;
    if (ops->read_environment != NULL || ops->read_temperature != NULL) {
        capabilities |= SOLAR_OS_SENSOR_CAP_TEMPERATURE;
    }
    if (ops->read_environment != NULL || ops->read_humidity != NULL) {
        capabilities |= SOLAR_OS_SENSOR_CAP_HUMIDITY;
    }
    return capabilities;
}

static bool sensor_has_capabilities(const sensor_device_t *device,
                                    solar_os_sensor_capabilities_t required)
{
    return (device->info.capabilities & required) == required;
}

static esp_err_t sensor_acquire(const char *name,
                                solar_os_sensor_capabilities_t required,
                                sensor_ref_t *ref)
{
    if (ref == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_ret = ensure_mutex();
    if (mutex_ret != ESP_OK) {
        return mutex_ret;
    }
    const bool named = name != NULL;
    if (named && !sensor_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = named ? ESP_ERR_NOT_FOUND : ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTake(sensors_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SENSOR_DEVICE_MAX; i++) {
        sensor_device_t *device = &sensor_devices[i];
        if (!device->active || device->detaching ||
            (named && strcmp(device->info.name, name) != 0)) {
            continue;
        }
        if (!sensor_has_capabilities(device, required)) {
            if (named) {
                ret = ESP_ERR_NOT_SUPPORTED;
                break;
            }
            continue;
        }
        device->refs++;
        *ref = (sensor_ref_t) {
            .index = i,
            .generation = device->generation,
            .ops = device->ops,
            .ctx = device->ctx,
        };
        ret = ESP_OK;
        break;
    }
    xSemaphoreGive(sensors_mutex);
    return ret;
}

static void sensor_release(const sensor_ref_t *ref)
{
    if (ref == NULL || ensure_mutex() != ESP_OK) {
        return;
    }
    xSemaphoreTake(sensors_mutex, portMAX_DELAY);
    if (ref->index < SENSOR_DEVICE_MAX) {
        sensor_device_t *device = &sensor_devices[ref->index];
        if (device->active && device->generation == ref->generation &&
            device->refs > 0U) {
            device->refs--;
        }
    }
    xSemaphoreGive(sensors_mutex);
}

static bool sensors_have_capability(solar_os_sensor_capabilities_t capability)
{
    if (ensure_mutex() != ESP_OK) {
        return false;
    }
    bool available = false;
    xSemaphoreTake(sensors_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SENSOR_DEVICE_MAX; i++) {
        if (sensor_devices[i].active && !sensor_devices[i].detaching &&
            sensor_has_capabilities(&sensor_devices[i], capability)) {
            available = true;
            break;
        }
    }
    xSemaphoreGive(sensors_mutex);
    return available;
}

static esp_err_t sensors_stream_read_scalar(
    void *user,
    const solar_os_stream_read_options_t *options,
    float *value)
{
    (void)options;
    return (uintptr_t)user == 0U ?
        solar_os_sensors_read_temperature(NULL, value) :
        solar_os_sensors_read_humidity(NULL, value);
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
    strlcpy(driver.info.device, "default", sizeof(driver.info.device));
    strlcpy(driver.info.unit, unit, sizeof(driver.info.unit));
    strlcpy(driver.info.format, "f32", sizeof(driver.info.format));
    strlcpy(driver.info.summary, summary, sizeof(driver.info.summary));
    return solar_os_stream_register(&driver);
}

static esp_err_t sensors_refresh_streams(void)
{
    const bool have_temperature =
        sensors_have_capability(SOLAR_OS_SENSOR_CAP_TEMPERATURE);
    const bool have_humidity =
        sensors_have_capability(SOLAR_OS_SENSOR_CAP_HUMIDITY);

    if (have_temperature && !temperature_stream_registered) {
        const esp_err_t ret = sensors_register_stream(
            "temperature", "C", "ambient temperature", 0U);
        if (ret != ESP_OK) {
            return ret;
        }
        temperature_stream_registered = true;
    } else if (!have_temperature && temperature_stream_registered) {
        const esp_err_t ret = solar_os_stream_unregister("temperature");
        if (ret != ESP_OK) {
            return ret;
        }
        temperature_stream_registered = false;
    }

    if (have_humidity && !humidity_stream_registered) {
        const esp_err_t ret = sensors_register_stream(
            "humidity", "percent", "relative humidity", 1U);
        if (ret != ESP_OK) {
            return ret;
        }
        humidity_stream_registered = true;
    } else if (!have_humidity && humidity_stream_registered) {
        const esp_err_t ret = solar_os_stream_unregister("humidity");
        if (ret != ESP_OK) {
            return ret;
        }
        humidity_stream_registered = false;
    }
    return ESP_OK;
}

static esp_err_t sensors_stream_require_idle(const char *id, bool registered)
{
    if (!registered) {
        return ESP_OK;
    }
    solar_os_stream_info_t info;
    const esp_err_t ret = solar_os_stream_get_info(id, &info);
    if (ret != ESP_OK) {
        return ret;
    }
    return info.active_handles == 0U ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_sensors_init(void)
{
    sensors_initialized = true;
    return sensors_refresh_streams();
}

esp_err_t solar_os_sensors_register(
    const solar_os_sensors_registration_t *registration)
{
    if (registration == NULL || !sensor_name_valid(registration->name) ||
        registration->driver == NULL || registration->ops == NULL ||
        provider_capabilities(registration->ops) == 0U ||
        strnlen(registration->driver, SOLAR_OS_SENSOR_DRIVER_MAX) >=
            SOLAR_OS_SENSOR_DRIVER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ensure_mutex() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(sensors_mutex, portMAX_DELAY);
    sensor_device_t *free_device = NULL;
    for (size_t i = 0; i < SENSOR_DEVICE_MAX; i++) {
        if (sensor_devices[i].active &&
            strcmp(sensor_devices[i].info.name, registration->name) == 0) {
            xSemaphoreGive(sensors_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        if (!sensor_devices[i].active && free_device == NULL) {
            free_device = &sensor_devices[i];
        }
    }
    if (free_device == NULL) {
        xSemaphoreGive(sensors_mutex);
        return ESP_ERR_NO_MEM;
    }

    memset(free_device, 0, sizeof(*free_device));
    free_device->active = true;
    strlcpy(free_device->info.name,
            registration->name,
            sizeof(free_device->info.name));
    strlcpy(free_device->info.driver,
            registration->driver,
            sizeof(free_device->info.driver));
    free_device->info.capabilities = provider_capabilities(registration->ops);
    free_device->ops = *registration->ops;
    free_device->ctx = registration->ctx;
    free_device->generation = sensors_next_generation++;
    if (free_device->generation == 0U) {
        free_device->generation = sensors_next_generation++;
    }
    xSemaphoreGive(sensors_mutex);

    if (sensors_initialized) {
        const esp_err_t ret = sensors_refresh_streams();
        if (ret != ESP_OK) {
            (void)solar_os_sensors_unregister(registration->name);
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_sensors_unregister(const char *name)
{
    if (!sensor_name_valid(name) || ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t remove_index = SENSOR_DEVICE_MAX;
    uint32_t remove_generation = 0U;
    bool remove_temperature_stream = false;
    bool remove_humidity_stream = false;

    xSemaphoreTake(sensors_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SENSOR_DEVICE_MAX; i++) {
        sensor_device_t *device = &sensor_devices[i];
        if (!device->active || strcmp(device->info.name, name) != 0) {
            continue;
        }
        if (device->refs > 0U) {
            xSemaphoreGive(sensors_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        remove_index = i;
        remove_generation = device->generation;
        remove_temperature_stream =
            sensor_has_capabilities(device, SOLAR_OS_SENSOR_CAP_TEMPERATURE);
        remove_humidity_stream =
            sensor_has_capabilities(device, SOLAR_OS_SENSOR_CAP_HUMIDITY);
        for (size_t j = 0; j < SENSOR_DEVICE_MAX; j++) {
            if (j == i || !sensor_devices[j].active ||
                sensor_devices[j].detaching) {
                continue;
            }
            remove_temperature_stream = remove_temperature_stream &&
                !sensor_has_capabilities(
                    &sensor_devices[j], SOLAR_OS_SENSOR_CAP_TEMPERATURE);
            remove_humidity_stream = remove_humidity_stream &&
                !sensor_has_capabilities(
                    &sensor_devices[j], SOLAR_OS_SENSOR_CAP_HUMIDITY);
        }
        device->detaching = true;
        break;
    }
    xSemaphoreGive(sensors_mutex);
    if (remove_index == SENSOR_DEVICE_MAX) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = sensors_stream_require_idle(
        "temperature", remove_temperature_stream && temperature_stream_registered);
    if (ret == ESP_OK) {
        ret = sensors_stream_require_idle(
            "humidity", remove_humidity_stream && humidity_stream_registered);
    }
    if (ret != ESP_OK) {
        xSemaphoreTake(sensors_mutex, portMAX_DELAY);
        sensor_device_t *device = &sensor_devices[remove_index];
        if (device->active && device->generation == remove_generation) {
            device->detaching = false;
        }
        xSemaphoreGive(sensors_mutex);
        return ret;
    }

    bool temperature_unregistered = false;
    if (remove_temperature_stream && temperature_stream_registered) {
        ret = solar_os_stream_unregister("temperature");
        if (ret == ESP_OK) {
            temperature_stream_registered = false;
            temperature_unregistered = true;
        }
    }
    if (ret == ESP_OK && remove_humidity_stream && humidity_stream_registered) {
        ret = solar_os_stream_unregister("humidity");
        if (ret == ESP_OK) {
            humidity_stream_registered = false;
        }
    }
    if (ret != ESP_OK) {
        if (temperature_unregistered &&
            sensors_register_stream(
                "temperature", "C", "ambient temperature", 0U) == ESP_OK) {
            temperature_stream_registered = true;
        }
        xSemaphoreTake(sensors_mutex, portMAX_DELAY);
        sensor_device_t *device = &sensor_devices[remove_index];
        if (device->active && device->generation == remove_generation) {
            device->detaching = false;
        }
        xSemaphoreGive(sensors_mutex);
        return ret;
    }

    xSemaphoreTake(sensors_mutex, portMAX_DELAY);
    sensor_device_t *device = &sensor_devices[remove_index];
    if (!device->active || !device->detaching ||
        device->generation != remove_generation ||
        device->refs > 0U) {
        if (device->active && device->generation == remove_generation) {
            device->detaching = false;
        }
        xSemaphoreGive(sensors_mutex);
        (void)sensors_refresh_streams();
        return ESP_ERR_INVALID_STATE;
    }
    memset(device, 0, sizeof(*device));
    xSemaphoreGive(sensors_mutex);

    return sensors_initialized ? sensors_refresh_streams() : ESP_OK;
}

size_t solar_os_sensors_count(void)
{
    if (ensure_mutex() != ESP_OK) {
        return 0U;
    }
    size_t count = 0U;
    xSemaphoreTake(sensors_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SENSOR_DEVICE_MAX; i++) {
        count += sensor_devices[i].active && !sensor_devices[i].detaching ? 1U : 0U;
    }
    xSemaphoreGive(sensors_mutex);
    return count;
}

bool solar_os_sensors_get(size_t index, solar_os_sensor_info_t *info)
{
    if (info == NULL || ensure_mutex() != ESP_OK) {
        return false;
    }
    size_t current = 0U;
    xSemaphoreTake(sensors_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SENSOR_DEVICE_MAX; i++) {
        if (!sensor_devices[i].active || sensor_devices[i].detaching) {
            continue;
        }
        if (current++ == index) {
            *info = sensor_devices[i].info;
            xSemaphoreGive(sensors_mutex);
            return true;
        }
    }
    xSemaphoreGive(sensors_mutex);
    return false;
}

bool solar_os_sensors_has_provider(void)
{
    return solar_os_sensors_count() > 0U;
}

static esp_err_t sensor_read_temperature(const sensor_ref_t *ref,
                                         float *temperature_c)
{
    if (ref->ops.read_temperature != NULL) {
        return ref->ops.read_temperature(ref->ctx, temperature_c);
    }
    solar_os_environment_t environment;
    const esp_err_t ret = ref->ops.read_environment(ref->ctx, &environment);
    if (ret == ESP_OK) {
        *temperature_c = environment.temperature_c;
    }
    return ret;
}

static esp_err_t sensor_read_humidity(const sensor_ref_t *ref,
                                      float *humidity_percent)
{
    if (ref->ops.read_humidity != NULL) {
        return ref->ops.read_humidity(ref->ctx, humidity_percent);
    }
    solar_os_environment_t environment;
    const esp_err_t ret = ref->ops.read_environment(ref->ctx, &environment);
    if (ret == ESP_OK) {
        *humidity_percent = environment.humidity_percent;
    }
    return ret;
}

esp_err_t solar_os_sensors_read_environment_from(
    const char *name,
    solar_os_environment_t *environment)
{
    if (!sensor_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (environment == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sensor_ref_t ref;
    const esp_err_t acquire_ret = sensor_acquire(
        name, SENSOR_CAP_ENVIRONMENT, &ref);
    if (acquire_ret != ESP_OK) {
        return acquire_ret;
    }
    esp_err_t ret;
    if (ref.ops.read_environment != NULL) {
        ret = ref.ops.read_environment(ref.ctx, environment);
    } else {
        ret = sensor_read_temperature(&ref, &environment->temperature_c);
        if (ret == ESP_OK) {
            ret = sensor_read_humidity(&ref, &environment->humidity_percent);
        }
    }
    sensor_release(&ref);
    return ret;
}

esp_err_t solar_os_sensors_read_temperature(const char *name, float *temperature_c)
{
    if (temperature_c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sensor_ref_t ref;
    const esp_err_t acquire_ret = sensor_acquire(
        name, SOLAR_OS_SENSOR_CAP_TEMPERATURE, &ref);
    if (acquire_ret != ESP_OK) {
        return acquire_ret;
    }
    const esp_err_t ret = sensor_read_temperature(&ref, temperature_c);
    sensor_release(&ref);
    return ret;
}

esp_err_t solar_os_sensors_read_humidity(const char *name, float *humidity_percent)
{
    if (humidity_percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sensor_ref_t ref;
    const esp_err_t acquire_ret = sensor_acquire(
        name, SOLAR_OS_SENSOR_CAP_HUMIDITY, &ref);
    if (acquire_ret != ESP_OK) {
        return acquire_ret;
    }
    const esp_err_t ret = sensor_read_humidity(&ref, humidity_percent);
    sensor_release(&ref);
    return ret;
}

esp_err_t solar_os_sensors_read_environment(solar_os_environment_t *environment)
{
    if (environment == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sensor_ref_t ref;
    const esp_err_t acquire_ret = sensor_acquire(
        NULL, SENSOR_CAP_ENVIRONMENT, &ref);
    if (acquire_ret == ESP_OK) {
        esp_err_t ret;
        if (ref.ops.read_environment != NULL) {
            ret = ref.ops.read_environment(ref.ctx, environment);
        } else {
            ret = sensor_read_temperature(&ref, &environment->temperature_c);
            if (ret == ESP_OK) {
                ret = sensor_read_humidity(&ref, &environment->humidity_percent);
            }
        }
        sensor_release(&ref);
        return ret;
    }
    if (acquire_ret != ESP_ERR_NOT_SUPPORTED) {
        return acquire_ret;
    }

    solar_os_environment_t result = {0};
    esp_err_t ret = solar_os_sensors_read_temperature(NULL, &result.temperature_c);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_sensors_read_humidity(NULL, &result.humidity_percent);
    if (ret != ESP_OK) {
        return ret;
    }
    *environment = result;
    return ESP_OK;
}
