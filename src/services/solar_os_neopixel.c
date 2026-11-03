#include "solar_os_neopixel.h"
#include "solar_os_sync_dynamic.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "neopixel.h"

#define SOLAR_OS_NEOPIXEL_MAX_DEVICES 4U

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    int data_pin;
    size_t pixel_count;
    uint8_t *grb;
    neopixel_t strip;
} solar_os_neopixel_device_t;

static const char *TAG = "neopixel";
static solar_os_neopixel_device_t devices[SOLAR_OS_NEOPIXEL_MAX_DEVICES];
static SemaphoreHandle_t devices_mutex;

static esp_err_t ensure_mutex(void)
{
    return solar_os_mutex_get_dynamic(&devices_mutex) != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static solar_os_neopixel_device_t *find_device_locked(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SOLAR_OS_NEOPIXEL_MAX_DEVICES; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

static solar_os_neopixel_device_t *alloc_device_locked(void)
{
    for (size_t i = 0; i < SOLAR_OS_NEOPIXEL_MAX_DEVICES; i++) {
        if (!devices[i].active) {
            return &devices[i];
        }
    }
    return NULL;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                int *data_pin,
                                size_t *pixel_count)
{
    bool have_data = false;
    bool have_count = false;
    if (bindings == NULL || data_pin == NULL || pixel_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
            strcmp(binding->role, "data") == 0 && !have_data) {
            *data_pin = binding->value;
            have_data = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "count") == 0 && !have_count &&
                   binding->value > 0 &&
                   binding->value <= (int)SOLAR_OS_NEOPIXEL_MAX_PIXELS) {
            *pixel_count = (size_t)binding->value;
            have_count = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_data && have_count ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t show_locked(solar_os_neopixel_device_t *device)
{
    return neopixel_write(&device->strip, device->grb, device->pixel_count * 3U);
}

esp_err_t solar_os_neopixel_attach(const char *name,
                                   const solar_os_expansion_binding_t *bindings,
                                   size_t binding_count)
{
    int data_pin = -1;
    size_t pixel_count = 0;
    if (name == NULL || name[0] == '\0' ||
        strnlen(name, SOLAR_OS_EXPANSION_DEVICE_NAME_MAX) >=
            SOLAR_OS_EXPANSION_DEVICE_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       &data_pin,
                                       &pixel_count),
                        TAG,
                        "invalid bindings");
    ESP_RETURN_ON_ERROR(ensure_mutex(), TAG, "mutex allocation failed");
    if (xSemaphoreTake(devices_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    solar_os_neopixel_device_t *device = NULL;
    if (find_device_locked(name) != NULL) {
        ret = ESP_ERR_INVALID_STATE;
        goto out;
    }
    device = alloc_device_locked();
    if (device == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto out;
    }
    memset(device, 0, sizeof(*device));
    device->data_pin = -1;
    device->grb = heap_caps_calloc(pixel_count,
                                   3U,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (device->grb == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    ret = neopixel_init(&device->strip, data_pin);
    if (ret != ESP_OK) {
        goto fail;
    }
    device->data_pin = data_pin;
    device->pixel_count = pixel_count;
    strlcpy(device->name, name, sizeof(device->name));
    device->active = true;
    ret = show_locked(device);
    if (ret != ESP_OK) {
        goto fail;
    }
    ESP_LOGI(TAG, "%s attached on GPIO%d with %u pixels",
             name, data_pin, (unsigned)pixel_count);
    goto out;

fail:
    if (device != NULL) {
        neopixel_deinit(&device->strip);
        free(device->grb);
        memset(device, 0, sizeof(*device));
        device->data_pin = -1;
    }
out:
    xSemaphoreGive(devices_mutex);
    return ret;
}

esp_err_t solar_os_neopixel_detach(const char *name)
{
    if (devices_mutex == NULL ||
        xSemaphoreTake(devices_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_NOT_FOUND;
    }
    solar_os_neopixel_device_t *device = find_device_locked(name);
    if (device == NULL) {
        xSemaphoreGive(devices_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    memset(device->grb, 0, device->pixel_count * 3U);
    const esp_err_t clear_ret = show_locked(device);
    if (clear_ret != ESP_OK) {
        ESP_LOGW(TAG, "%s clear before detach failed: %s",
                 device->name, esp_err_to_name(clear_ret));
    }
    neopixel_deinit(&device->strip);
    free(device->grb);
    memset(device, 0, sizeof(*device));
    device->data_pin = -1;
    xSemaphoreGive(devices_mutex);
    return ESP_OK;
}

size_t solar_os_neopixel_count(void)
{
    if (devices_mutex == NULL ||
        xSemaphoreTake(devices_mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < SOLAR_OS_NEOPIXEL_MAX_DEVICES; i++) {
        count += devices[i].active ? 1U : 0U;
    }
    xSemaphoreGive(devices_mutex);
    return count;
}

bool solar_os_neopixel_get(size_t index, solar_os_neopixel_info_t *info)
{
    if (info == NULL || devices_mutex == NULL ||
        xSemaphoreTake(devices_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    size_t current = 0;
    bool found = false;
    for (size_t i = 0; i < SOLAR_OS_NEOPIXEL_MAX_DEVICES; i++) {
        if (!devices[i].active) {
            continue;
        }
        if (current++ == index) {
            memset(info, 0, sizeof(*info));
            strlcpy(info->name, devices[i].name, sizeof(info->name));
            info->data_pin = devices[i].data_pin;
            info->pixel_count = devices[i].pixel_count;
            found = true;
            break;
        }
    }
    xSemaphoreGive(devices_mutex);
    return found;
}

esp_err_t solar_os_neopixel_set(const char *name,
                               size_t index,
                               uint8_t red,
                               uint8_t green,
                               uint8_t blue)
{
    if (devices_mutex == NULL ||
        xSemaphoreTake(devices_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_NOT_FOUND;
    }
    solar_os_neopixel_device_t *device = find_device_locked(name);
    if (device == NULL || index >= device->pixel_count) {
        xSemaphoreGive(devices_mutex);
        return device == NULL ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_ARG;
    }
    device->grb[index * 3U] = green;
    device->grb[index * 3U + 1U] = red;
    device->grb[index * 3U + 2U] = blue;
    xSemaphoreGive(devices_mutex);
    return ESP_OK;
}

esp_err_t solar_os_neopixel_fill(const char *name,
                                uint8_t red,
                                uint8_t green,
                                uint8_t blue)
{
    if (devices_mutex == NULL ||
        xSemaphoreTake(devices_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_NOT_FOUND;
    }
    solar_os_neopixel_device_t *device = find_device_locked(name);
    if (device == NULL) {
        xSemaphoreGive(devices_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t i = 0; i < device->pixel_count; i++) {
        device->grb[i * 3U] = green;
        device->grb[i * 3U + 1U] = red;
        device->grb[i * 3U + 2U] = blue;
    }
    xSemaphoreGive(devices_mutex);
    return ESP_OK;
}

esp_err_t solar_os_neopixel_show(const char *name)
{
    if (devices_mutex == NULL ||
        xSemaphoreTake(devices_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_NOT_FOUND;
    }
    solar_os_neopixel_device_t *device = find_device_locked(name);
    const esp_err_t ret = device != NULL ? show_locked(device) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(devices_mutex);
    return ret;
}

esp_err_t solar_os_neopixel_clear(const char *name)
{
    if (devices_mutex == NULL ||
        xSemaphoreTake(devices_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_NOT_FOUND;
    }
    solar_os_neopixel_device_t *device = find_device_locked(name);
    if (device == NULL) {
        xSemaphoreGive(devices_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    memset(device->grb, 0, device->pixel_count * 3U);
    const esp_err_t ret = show_locked(device);
    xSemaphoreGive(devices_mutex);
    return ret;
}
