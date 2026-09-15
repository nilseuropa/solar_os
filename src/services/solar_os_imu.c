#include "solar_os_imu.h"

#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define IMU_DEVICE_MAX 3U
#define IMU_CAP_ALL (SOLAR_OS_IMU_CAP_ACCELERATION | \
                     SOLAR_OS_IMU_CAP_ANGULAR_VELOCITY | \
                     SOLAR_OS_IMU_CAP_ORIENTATION)

typedef struct {
    bool active;
    solar_os_imu_info_t info;
    solar_os_imu_ops_t ops;
    void *ctx;
    size_t refs;
    uint32_t generation;
} imu_device_t;

static SemaphoreHandle_t imu_mutex;
static EXT_RAM_BSS_ATTR StaticSemaphore_t imu_mutex_storage;
static EXT_RAM_BSS_ATTR imu_device_t imu_devices[IMU_DEVICE_MAX];
static uint32_t imu_next_generation = 1U;

static esp_err_t ensure_mutex(void)
{
    if (imu_mutex == NULL) {
        imu_mutex = xSemaphoreCreateMutexStatic(&imu_mutex_storage);
    }
    return imu_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool name_valid(const char *name)
{
    return name != NULL && name[0] != '\0' &&
        strnlen(name, SOLAR_OS_IMU_NAME_MAX) < SOLAR_OS_IMU_NAME_MAX;
}

esp_err_t solar_os_imu_register(const solar_os_imu_registration_t *registration)
{
    if (registration == NULL || !name_valid(registration->name) ||
        registration->driver == NULL || registration->ops == NULL ||
        registration->ops->read_sample == NULL ||
        registration->capabilities == 0U ||
        (registration->capabilities & ~IMU_CAP_ALL) != 0U ||
        strnlen(registration->driver, SOLAR_OS_IMU_DRIVER_MAX) >=
            SOLAR_OS_IMU_DRIVER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_ret = ensure_mutex();
    if (mutex_ret != ESP_OK) {
        return mutex_ret;
    }

    xSemaphoreTake(imu_mutex, portMAX_DELAY);
    imu_device_t *free_device = NULL;
    for (size_t i = 0; i < IMU_DEVICE_MAX; i++) {
        if (imu_devices[i].active &&
            strcmp(imu_devices[i].info.name, registration->name) == 0) {
            xSemaphoreGive(imu_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        if (!imu_devices[i].active && free_device == NULL) {
            free_device = &imu_devices[i];
        }
    }
    if (free_device == NULL) {
        xSemaphoreGive(imu_mutex);
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
    free_device->info.capabilities = registration->capabilities;
    free_device->ops = *registration->ops;
    free_device->ctx = registration->ctx;
    free_device->generation = imu_next_generation++;
    if (free_device->generation == 0U) {
        free_device->generation = imu_next_generation++;
    }
    xSemaphoreGive(imu_mutex);
    return ESP_OK;
}

esp_err_t solar_os_imu_unregister(const char *name)
{
    if (!name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_ret = ensure_mutex();
    if (mutex_ret != ESP_OK) {
        return mutex_ret;
    }

    xSemaphoreTake(imu_mutex, portMAX_DELAY);
    for (size_t i = 0; i < IMU_DEVICE_MAX; i++) {
        if (imu_devices[i].active &&
            strcmp(imu_devices[i].info.name, name) == 0) {
            if (imu_devices[i].refs > 0U) {
                xSemaphoreGive(imu_mutex);
                return ESP_ERR_INVALID_STATE;
            }
            memset(&imu_devices[i], 0, sizeof(imu_devices[i]));
            xSemaphoreGive(imu_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(imu_mutex);
    return ESP_ERR_NOT_FOUND;
}

size_t solar_os_imu_count(void)
{
    if (ensure_mutex() != ESP_OK) {
        return 0U;
    }
    size_t count = 0U;
    xSemaphoreTake(imu_mutex, portMAX_DELAY);
    for (size_t i = 0; i < IMU_DEVICE_MAX; i++) {
        count += imu_devices[i].active ? 1U : 0U;
    }
    xSemaphoreGive(imu_mutex);
    return count;
}

bool solar_os_imu_get(size_t index, solar_os_imu_info_t *info)
{
    if (info == NULL || ensure_mutex() != ESP_OK) {
        return false;
    }
    size_t current = 0U;
    xSemaphoreTake(imu_mutex, portMAX_DELAY);
    for (size_t i = 0; i < IMU_DEVICE_MAX; i++) {
        if (!imu_devices[i].active) {
            continue;
        }
        if (current++ == index) {
            *info = imu_devices[i].info;
            xSemaphoreGive(imu_mutex);
            return true;
        }
    }
    xSemaphoreGive(imu_mutex);
    return false;
}

esp_err_t solar_os_imu_read_sample(const char *name,
                                   uint32_t timeout_ms,
                                   solar_os_imu_sample_t *sample)
{
    if (!name_valid(name) || sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_ret = ensure_mutex();
    if (mutex_ret != ESP_OK) {
        return mutex_ret;
    }

    solar_os_imu_ops_t ops = {0};
    void *ctx = NULL;
    size_t index = 0U;
    uint32_t generation = 0U;
    solar_os_imu_capabilities_t capabilities = 0U;
    xSemaphoreTake(imu_mutex, portMAX_DELAY);
    for (size_t i = 0; i < IMU_DEVICE_MAX; i++) {
        if (imu_devices[i].active &&
            strcmp(imu_devices[i].info.name, name) == 0) {
            imu_devices[i].refs++;
            ops = imu_devices[i].ops;
            ctx = imu_devices[i].ctx;
            index = i;
            generation = imu_devices[i].generation;
            capabilities = imu_devices[i].info.capabilities;
            break;
        }
    }
    xSemaphoreGive(imu_mutex);
    if (ops.read_sample == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(sample, 0, sizeof(*sample));
    const esp_err_t ret = ops.read_sample(ctx, timeout_ms, sample);
    if (ret == ESP_OK && (sample->valid & ~capabilities) != 0U) {
        sample->valid &= capabilities;
    }

    xSemaphoreTake(imu_mutex, portMAX_DELAY);
    if (imu_devices[index].active &&
        imu_devices[index].generation == generation &&
        imu_devices[index].refs > 0U) {
        imu_devices[index].refs--;
    }
    xSemaphoreGive(imu_mutex);
    return ret;
}
