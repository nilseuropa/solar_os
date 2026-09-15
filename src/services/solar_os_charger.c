#include "solar_os_charger.h"

#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define CHARGER_DEVICE_MAX 3U

typedef struct {
    bool active;
    solar_os_charger_info_t info;
    solar_os_charger_ops_t ops;
    void *ctx;
    size_t refs;
    uint32_t generation;
} charger_device_t;

typedef struct {
    size_t index;
    uint32_t generation;
    solar_os_charger_info_t info;
    solar_os_charger_ops_t ops;
    void *ctx;
} charger_ref_t;

static EXT_RAM_BSS_ATTR SemaphoreHandle_t charger_mutex;
static EXT_RAM_BSS_ATTR StaticSemaphore_t charger_mutex_storage;
static EXT_RAM_BSS_ATTR charger_device_t charger_devices[CHARGER_DEVICE_MAX];
static EXT_RAM_BSS_ATTR uint32_t charger_next_generation;

static esp_err_t ensure_mutex(void)
{
    if (charger_mutex == NULL) {
        charger_mutex = xSemaphoreCreateMutexStatic(&charger_mutex_storage);
    }
    return charger_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool name_valid(const char *name)
{
    return name != NULL && name[0] != '\0' &&
        strnlen(name, SOLAR_OS_CHARGER_NAME_MAX) < SOLAR_OS_CHARGER_NAME_MAX;
}

static bool range_valid(const solar_os_charger_range_t *range)
{
    return range->minimum <= range->maximum && range->step > 0U &&
        ((uint32_t)range->maximum - range->minimum) % range->step == 0U;
}

static bool value_valid(const solar_os_charger_range_t *range, uint16_t value)
{
    return value >= range->minimum && value <= range->maximum &&
        ((uint32_t)value - range->minimum) % range->step == 0U;
}

static esp_err_t acquire(const char *name, charger_ref_t *ref)
{
    if (!name_valid(name) || ref == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_ret = ensure_mutex();
    if (mutex_ret != ESP_OK) {
        return mutex_ret;
    }
    memset(ref, 0, sizeof(*ref));
    xSemaphoreTake(charger_mutex, portMAX_DELAY);
    for (size_t i = 0; i < CHARGER_DEVICE_MAX; i++) {
        if (!charger_devices[i].active ||
            strcmp(charger_devices[i].info.name, name) != 0) {
            continue;
        }
        charger_devices[i].refs++;
        ref->index = i;
        ref->generation = charger_devices[i].generation;
        ref->info = charger_devices[i].info;
        ref->ops = charger_devices[i].ops;
        ref->ctx = charger_devices[i].ctx;
        xSemaphoreGive(charger_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(charger_mutex);
    return ESP_ERR_NOT_FOUND;
}

static void release(const charger_ref_t *ref)
{
    xSemaphoreTake(charger_mutex, portMAX_DELAY);
    if (charger_devices[ref->index].active &&
        charger_devices[ref->index].generation == ref->generation &&
        charger_devices[ref->index].refs > 0U) {
        charger_devices[ref->index].refs--;
    }
    xSemaphoreGive(charger_mutex);
}

esp_err_t solar_os_charger_register(
    const solar_os_charger_registration_t *registration)
{
    if (registration == NULL || !name_valid(registration->name) ||
        registration->driver == NULL || registration->ops == NULL ||
        registration->ops->read_status == NULL ||
        registration->ops->set_enabled == NULL ||
        registration->ops->set_input_current_limit == NULL ||
        registration->ops->set_charge_current == NULL ||
        registration->ops->set_charge_voltage == NULL ||
        !range_valid(&registration->input_current_limit_ma) ||
        !range_valid(&registration->charge_current_ma) ||
        !range_valid(&registration->charge_voltage_mv) ||
        strnlen(registration->driver, SOLAR_OS_CHARGER_DRIVER_MAX) >=
            SOLAR_OS_CHARGER_DRIVER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_ret = ensure_mutex();
    if (mutex_ret != ESP_OK) {
        return mutex_ret;
    }

    xSemaphoreTake(charger_mutex, portMAX_DELAY);
    charger_device_t *free_device = NULL;
    for (size_t i = 0; i < CHARGER_DEVICE_MAX; i++) {
        if (charger_devices[i].active &&
            strcmp(charger_devices[i].info.name, registration->name) == 0) {
            xSemaphoreGive(charger_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        if (!charger_devices[i].active && free_device == NULL) {
            free_device = &charger_devices[i];
        }
    }
    if (free_device == NULL) {
        xSemaphoreGive(charger_mutex);
        return ESP_ERR_NO_MEM;
    }

    memset(free_device, 0, sizeof(*free_device));
    free_device->active = true;
    strlcpy(free_device->info.name, registration->name,
            sizeof(free_device->info.name));
    strlcpy(free_device->info.driver, registration->driver,
            sizeof(free_device->info.driver));
    free_device->info.input_current_limit_ma =
        registration->input_current_limit_ma;
    free_device->info.charge_current_ma = registration->charge_current_ma;
    free_device->info.charge_voltage_mv = registration->charge_voltage_mv;
    free_device->ops = *registration->ops;
    free_device->ctx = registration->ctx;
    free_device->generation = ++charger_next_generation;
    if (free_device->generation == 0U) {
        free_device->generation = ++charger_next_generation;
    }
    xSemaphoreGive(charger_mutex);
    return ESP_OK;
}

esp_err_t solar_os_charger_unregister(const char *name)
{
    if (!name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_ret = ensure_mutex();
    if (mutex_ret != ESP_OK) {
        return mutex_ret;
    }
    xSemaphoreTake(charger_mutex, portMAX_DELAY);
    for (size_t i = 0; i < CHARGER_DEVICE_MAX; i++) {
        if (!charger_devices[i].active ||
            strcmp(charger_devices[i].info.name, name) != 0) {
            continue;
        }
        if (charger_devices[i].refs > 0U) {
            xSemaphoreGive(charger_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        memset(&charger_devices[i], 0, sizeof(charger_devices[i]));
        xSemaphoreGive(charger_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(charger_mutex);
    return ESP_ERR_NOT_FOUND;
}

size_t solar_os_charger_count(void)
{
    if (ensure_mutex() != ESP_OK) {
        return 0U;
    }
    size_t count = 0U;
    xSemaphoreTake(charger_mutex, portMAX_DELAY);
    for (size_t i = 0; i < CHARGER_DEVICE_MAX; i++) {
        count += charger_devices[i].active ? 1U : 0U;
    }
    xSemaphoreGive(charger_mutex);
    return count;
}

bool solar_os_charger_get(size_t index, solar_os_charger_info_t *info)
{
    if (info == NULL || ensure_mutex() != ESP_OK) {
        return false;
    }
    size_t current = 0U;
    xSemaphoreTake(charger_mutex, portMAX_DELAY);
    for (size_t i = 0; i < CHARGER_DEVICE_MAX; i++) {
        if (!charger_devices[i].active) {
            continue;
        }
        if (current++ == index) {
            *info = charger_devices[i].info;
            xSemaphoreGive(charger_mutex);
            return true;
        }
    }
    xSemaphoreGive(charger_mutex);
    return false;
}

esp_err_t solar_os_charger_read_status(const char *name,
                                       solar_os_charger_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    charger_ref_t ref;
    const esp_err_t acquire_ret = acquire(name, &ref);
    if (acquire_ret != ESP_OK) {
        return acquire_ret;
    }
    const esp_err_t ret = ref.ops.read_status(ref.ctx, status);
    release(&ref);
    return ret;
}

esp_err_t solar_os_charger_set_enabled(const char *name, bool enabled)
{
    charger_ref_t ref;
    const esp_err_t acquire_ret = acquire(name, &ref);
    if (acquire_ret != ESP_OK) {
        return acquire_ret;
    }
    const esp_err_t ret = ref.ops.set_enabled(ref.ctx, enabled);
    release(&ref);
    return ret;
}

esp_err_t solar_os_charger_set_input_current_limit(const char *name,
                                                   uint16_t current_ma)
{
    charger_ref_t ref;
    const esp_err_t acquire_ret = acquire(name, &ref);
    if (acquire_ret != ESP_OK) {
        return acquire_ret;
    }
    const esp_err_t ret = value_valid(&ref.info.input_current_limit_ma,
                                      current_ma) ?
        ref.ops.set_input_current_limit(ref.ctx, current_ma) :
        ESP_ERR_INVALID_ARG;
    release(&ref);
    return ret;
}

esp_err_t solar_os_charger_set_charge_current(const char *name,
                                              uint16_t current_ma)
{
    charger_ref_t ref;
    const esp_err_t acquire_ret = acquire(name, &ref);
    if (acquire_ret != ESP_OK) {
        return acquire_ret;
    }
    const esp_err_t ret = value_valid(&ref.info.charge_current_ma, current_ma) ?
        ref.ops.set_charge_current(ref.ctx, current_ma) : ESP_ERR_INVALID_ARG;
    release(&ref);
    return ret;
}

esp_err_t solar_os_charger_set_charge_voltage(const char *name,
                                              uint16_t voltage_mv)
{
    charger_ref_t ref;
    const esp_err_t acquire_ret = acquire(name, &ref);
    if (acquire_ret != ESP_OK) {
        return acquire_ret;
    }
    const esp_err_t ret = value_valid(&ref.info.charge_voltage_mv, voltage_mv) ?
        ref.ops.set_charge_voltage(ref.ctx, voltage_mv) : ESP_ERR_INVALID_ARG;
    release(&ref);
    return ret;
}

const char *solar_os_charger_state_name(solar_os_charger_state_t state)
{
    switch (state) {
    case SOLAR_OS_CHARGER_STATE_NOT_CHARGING: return "not-charging";
    case SOLAR_OS_CHARGER_STATE_PRECHARGE: return "precharge";
    case SOLAR_OS_CHARGER_STATE_FAST_CHARGE: return "fast-charge";
    case SOLAR_OS_CHARGER_STATE_DONE: return "done";
    default: return "unknown";
    }
}
