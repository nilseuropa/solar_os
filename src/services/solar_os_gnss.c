#include "solar_os_gnss.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define GNSS_DEVICE_MAX 3U

typedef struct {
    bool active;
    solar_os_gnss_info_t info;
    const solar_os_gnss_ops_t *ops;
    void *ctx;
    size_t refs;
    uint32_t generation;
} gnss_device_t;

static SemaphoreHandle_t gnss_mutex;
static StaticSemaphore_t gnss_mutex_storage;
static gnss_device_t gnss_devices[GNSS_DEVICE_MAX];
static uint32_t gnss_next_generation = 1U;

static esp_err_t ensure_mutex(void)
{
    if (gnss_mutex == NULL) {
        gnss_mutex = xSemaphoreCreateMutexStatic(&gnss_mutex_storage);
    }
    return gnss_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool name_valid(const char *name)
{
    return name != NULL && name[0] != '\0' &&
        strnlen(name, SOLAR_OS_GNSS_NAME_MAX) < SOLAR_OS_GNSS_NAME_MAX;
}

esp_err_t solar_os_gnss_register(const solar_os_gnss_registration_t *registration)
{
    if (registration == NULL || !name_valid(registration->name) ||
        registration->driver == NULL || registration->ops == NULL ||
        registration->ops->read_fix == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strnlen(registration->driver, SOLAR_OS_GNSS_DRIVER_MAX) >=
        SOLAR_OS_GNSS_DRIVER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ensure_mutex() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(gnss_mutex, portMAX_DELAY);
    gnss_device_t *free_device = NULL;
    for (size_t i = 0; i < GNSS_DEVICE_MAX; i++) {
        if (gnss_devices[i].active &&
            strcmp(gnss_devices[i].info.name, registration->name) == 0) {
            xSemaphoreGive(gnss_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        if (!gnss_devices[i].active && free_device == NULL) {
            free_device = &gnss_devices[i];
        }
    }
    if (free_device == NULL) {
        xSemaphoreGive(gnss_mutex);
        return ESP_ERR_NO_MEM;
    }
    memset(free_device, 0, sizeof(*free_device));
    free_device->active = true;
    strlcpy(free_device->info.name, registration->name, sizeof(free_device->info.name));
    strlcpy(free_device->info.driver, registration->driver, sizeof(free_device->info.driver));
    free_device->ops = registration->ops;
    free_device->ctx = registration->ctx;
    free_device->generation = gnss_next_generation++;
    if (free_device->generation == 0U) {
        free_device->generation = gnss_next_generation++;
    }
    xSemaphoreGive(gnss_mutex);
    return ESP_OK;
}

esp_err_t solar_os_gnss_unregister(const char *name)
{
    if (!name_valid(name) || ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(gnss_mutex, portMAX_DELAY);
    for (size_t i = 0; i < GNSS_DEVICE_MAX; i++) {
        if (gnss_devices[i].active && strcmp(gnss_devices[i].info.name, name) == 0) {
            if (gnss_devices[i].refs > 0U) {
                xSemaphoreGive(gnss_mutex);
                return ESP_ERR_INVALID_STATE;
            }
            memset(&gnss_devices[i], 0, sizeof(gnss_devices[i]));
            xSemaphoreGive(gnss_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(gnss_mutex);
    return ESP_ERR_NOT_FOUND;
}

size_t solar_os_gnss_count(void)
{
    if (ensure_mutex() != ESP_OK) {
        return 0U;
    }
    size_t count = 0U;
    xSemaphoreTake(gnss_mutex, portMAX_DELAY);
    for (size_t i = 0; i < GNSS_DEVICE_MAX; i++) {
        count += gnss_devices[i].active ? 1U : 0U;
    }
    xSemaphoreGive(gnss_mutex);
    return count;
}

bool solar_os_gnss_get(size_t index, solar_os_gnss_info_t *info)
{
    if (info == NULL || ensure_mutex() != ESP_OK) {
        return false;
    }
    size_t current = 0U;
    xSemaphoreTake(gnss_mutex, portMAX_DELAY);
    for (size_t i = 0; i < GNSS_DEVICE_MAX; i++) {
        if (!gnss_devices[i].active) {
            continue;
        }
        if (current++ == index) {
            *info = gnss_devices[i].info;
            xSemaphoreGive(gnss_mutex);
            return true;
        }
    }
    xSemaphoreGive(gnss_mutex);
    return false;
}

esp_err_t solar_os_gnss_read_fix(const char *name,
                                 uint32_t timeout_ms,
                                 solar_os_gnss_fix_t *fix)
{
    if (!name_valid(name) || fix == NULL || ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    const solar_os_gnss_ops_t *ops = NULL;
    void *ctx = NULL;
    size_t index = 0U;
    uint32_t generation = 0U;
    xSemaphoreTake(gnss_mutex, portMAX_DELAY);
    for (size_t i = 0; i < GNSS_DEVICE_MAX; i++) {
        if (gnss_devices[i].active && strcmp(gnss_devices[i].info.name, name) == 0) {
            gnss_devices[i].refs++;
            ops = gnss_devices[i].ops;
            ctx = gnss_devices[i].ctx;
            index = i;
            generation = gnss_devices[i].generation;
            break;
        }
    }
    xSemaphoreGive(gnss_mutex);
    if (ops == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    const esp_err_t ret = ops->read_fix(ctx, timeout_ms, fix);

    xSemaphoreTake(gnss_mutex, portMAX_DELAY);
    if (gnss_devices[index].active &&
        gnss_devices[index].generation == generation &&
        gnss_devices[index].refs > 0U) {
        gnss_devices[index].refs--;
    }
    xSemaphoreGive(gnss_mutex);
    return ret;
}
