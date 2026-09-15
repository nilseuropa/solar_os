#include "solar_os_haptic.h"

#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define HAPTIC_DEVICE_MAX 3U

typedef struct {
    bool active;
    solar_os_haptic_info_t info;
    solar_os_haptic_ops_t ops;
    void *ctx;
    size_t refs;
    uint32_t generation;
} haptic_device_t;

typedef struct {
    size_t index;
    uint32_t generation;
    uint16_t effect_count;
    solar_os_haptic_ops_t ops;
    void *ctx;
} haptic_ref_t;

static SemaphoreHandle_t haptic_mutex;
static EXT_RAM_BSS_ATTR StaticSemaphore_t haptic_mutex_storage;
static EXT_RAM_BSS_ATTR haptic_device_t haptic_devices[HAPTIC_DEVICE_MAX];
static uint32_t haptic_next_generation = 1U;

static esp_err_t ensure_mutex(void)
{
    if (haptic_mutex == NULL) {
        haptic_mutex = xSemaphoreCreateMutexStatic(&haptic_mutex_storage);
    }
    return haptic_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool name_valid(const char *name)
{
    return name != NULL && name[0] != '\0' &&
        strnlen(name, SOLAR_OS_HAPTIC_NAME_MAX) < SOLAR_OS_HAPTIC_NAME_MAX;
}

static esp_err_t acquire(const char *name, haptic_ref_t *ref)
{
    if (!name_valid(name) || ref == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_ret = ensure_mutex();
    if (mutex_ret != ESP_OK) {
        return mutex_ret;
    }
    memset(ref, 0, sizeof(*ref));
    xSemaphoreTake(haptic_mutex, portMAX_DELAY);
    for (size_t i = 0; i < HAPTIC_DEVICE_MAX; i++) {
        if (!haptic_devices[i].active ||
            strcmp(haptic_devices[i].info.name, name) != 0) {
            continue;
        }
        haptic_devices[i].refs++;
        ref->index = i;
        ref->generation = haptic_devices[i].generation;
        ref->effect_count = haptic_devices[i].info.effect_count;
        ref->ops = haptic_devices[i].ops;
        ref->ctx = haptic_devices[i].ctx;
        xSemaphoreGive(haptic_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(haptic_mutex);
    return ESP_ERR_NOT_FOUND;
}

static void release(const haptic_ref_t *ref)
{
    xSemaphoreTake(haptic_mutex, portMAX_DELAY);
    if (haptic_devices[ref->index].active &&
        haptic_devices[ref->index].generation == ref->generation &&
        haptic_devices[ref->index].refs > 0U) {
        haptic_devices[ref->index].refs--;
    }
    xSemaphoreGive(haptic_mutex);
}

esp_err_t solar_os_haptic_register(
    const solar_os_haptic_registration_t *registration)
{
    if (registration == NULL || !name_valid(registration->name) ||
        registration->driver == NULL || registration->ops == NULL ||
        registration->ops->play_effect == NULL ||
        registration->ops->stop == NULL || registration->effect_count == 0U ||
        strnlen(registration->driver, SOLAR_OS_HAPTIC_DRIVER_MAX) >=
            SOLAR_OS_HAPTIC_DRIVER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_ret = ensure_mutex();
    if (mutex_ret != ESP_OK) {
        return mutex_ret;
    }

    xSemaphoreTake(haptic_mutex, portMAX_DELAY);
    haptic_device_t *free_device = NULL;
    for (size_t i = 0; i < HAPTIC_DEVICE_MAX; i++) {
        if (haptic_devices[i].active &&
            strcmp(haptic_devices[i].info.name, registration->name) == 0) {
            xSemaphoreGive(haptic_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        if (!haptic_devices[i].active && free_device == NULL) {
            free_device = &haptic_devices[i];
        }
    }
    if (free_device == NULL) {
        xSemaphoreGive(haptic_mutex);
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
    free_device->info.effect_count = registration->effect_count;
    free_device->ops = *registration->ops;
    free_device->ctx = registration->ctx;
    free_device->generation = haptic_next_generation++;
    if (free_device->generation == 0U) {
        free_device->generation = haptic_next_generation++;
    }
    xSemaphoreGive(haptic_mutex);
    return ESP_OK;
}

esp_err_t solar_os_haptic_unregister(const char *name)
{
    if (!name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_ret = ensure_mutex();
    if (mutex_ret != ESP_OK) {
        return mutex_ret;
    }
    xSemaphoreTake(haptic_mutex, portMAX_DELAY);
    for (size_t i = 0; i < HAPTIC_DEVICE_MAX; i++) {
        if (!haptic_devices[i].active ||
            strcmp(haptic_devices[i].info.name, name) != 0) {
            continue;
        }
        if (haptic_devices[i].refs > 0U) {
            xSemaphoreGive(haptic_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        memset(&haptic_devices[i], 0, sizeof(haptic_devices[i]));
        xSemaphoreGive(haptic_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(haptic_mutex);
    return ESP_ERR_NOT_FOUND;
}

size_t solar_os_haptic_count(void)
{
    if (ensure_mutex() != ESP_OK) {
        return 0U;
    }
    size_t count = 0U;
    xSemaphoreTake(haptic_mutex, portMAX_DELAY);
    for (size_t i = 0; i < HAPTIC_DEVICE_MAX; i++) {
        count += haptic_devices[i].active ? 1U : 0U;
    }
    xSemaphoreGive(haptic_mutex);
    return count;
}

bool solar_os_haptic_get(size_t index, solar_os_haptic_info_t *info)
{
    if (info == NULL || ensure_mutex() != ESP_OK) {
        return false;
    }
    size_t current = 0U;
    xSemaphoreTake(haptic_mutex, portMAX_DELAY);
    for (size_t i = 0; i < HAPTIC_DEVICE_MAX; i++) {
        if (!haptic_devices[i].active) {
            continue;
        }
        if (current++ == index) {
            *info = haptic_devices[i].info;
            xSemaphoreGive(haptic_mutex);
            return true;
        }
    }
    xSemaphoreGive(haptic_mutex);
    return false;
}

esp_err_t solar_os_haptic_play_effect(const char *name, uint16_t effect)
{
    haptic_ref_t ref;
    const esp_err_t acquire_ret = acquire(name, &ref);
    if (acquire_ret != ESP_OK) {
        return acquire_ret;
    }

    if (effect == 0U || effect > ref.effect_count) {
        release(&ref);
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t ret = ref.ops.play_effect(ref.ctx, effect);
    release(&ref);
    return ret;
}

esp_err_t solar_os_haptic_stop(const char *name)
{
    haptic_ref_t ref;
    const esp_err_t acquire_ret = acquire(name, &ref);
    if (acquire_ret != ESP_OK) {
        return acquire_ret;
    }
    const esp_err_t ret = ref.ops.stop(ref.ctx);
    release(&ref);
    return ret;
}
