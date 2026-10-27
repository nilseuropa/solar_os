#include "solar_os_motor.h"

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_memory.h"

typedef struct motor {
    struct motor *next;
    solar_os_motor_info_t info;
    solar_os_motor_ops_t ops;
    void *ctx;
    size_t refs;
    bool removing;
    StaticSemaphore_t *operation_storage;
    SemaphoreHandle_t operation_mutex;
} motor_t;

static motor_t *motors;
static StaticSemaphore_t registry_storage;
static SemaphoreHandle_t registry_mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;

static void lock(void)
{
    portENTER_CRITICAL(&init_lock);
    if (registry_mutex == NULL) registry_mutex = xSemaphoreCreateMutexStatic(&registry_storage);
    portEXIT_CRITICAL(&init_lock);
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
}

static bool valid_name(const char *name, size_t maximum)
{
    return name != NULL && name[0] != '\0' && strnlen(name, maximum) < maximum;
}

static motor_t *find(const char *name)
{
    for (motor_t *m = motors; m != NULL; m = m->next) {
        if (strcmp(m->info.name, name) == 0) return m;
    }
    return NULL;
}

static esp_err_t acquire(const char *name, motor_t **result)
{
    if (!valid_name(name, SOLAR_OS_MOTOR_NAME_MAX)) return ESP_ERR_INVALID_ARG;
    lock();
    motor_t *m = find(name);
    esp_err_t ret = m == NULL ? ESP_ERR_NOT_FOUND : m->removing ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (ret == ESP_OK) m->refs++;
    xSemaphoreGive(registry_mutex);
    if (ret == ESP_OK) {
        xSemaphoreTake(m->operation_mutex, portMAX_DELAY);
        *result = m;
    }
    return ret;
}

static void release(motor_t *m)
{
    xSemaphoreGive(m->operation_mutex);
    lock();
    m->refs--;
    xSemaphoreGive(registry_mutex);
}

esp_err_t solar_os_motor_register(const solar_os_motor_registration_t *registration)
{
    if (registration == NULL || !valid_name(registration->name, SOLAR_OS_MOTOR_NAME_MAX) ||
        !valid_name(registration->driver, SOLAR_OS_MOTOR_DRIVER_MAX) || registration->ops == NULL ||
        registration->ops->status == NULL || registration->ops->coast == NULL) return ESP_ERR_INVALID_ARG;
    lock();
    if (find(registration->name) != NULL) { xSemaphoreGive(registry_mutex); return ESP_ERR_INVALID_STATE; }
    motor_t *m = solar_os_memory_calloc(1U, sizeof(*m), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "motor.device");
    if (m == NULL) { xSemaphoreGive(registry_mutex); return ESP_ERR_NO_MEM; }
    /* Scheduler objects must stay in internal RAM even when metadata uses PSRAM. */
    m->operation_storage = solar_os_memory_calloc(1U, sizeof(*m->operation_storage),
        SOLAR_OS_MEMORY_INTERNAL_CRITICAL, "motor.lock");
    if (m->operation_storage != NULL) m->operation_mutex = xSemaphoreCreateMutexStatic(m->operation_storage);
    if (m->operation_mutex == NULL) {
        solar_os_memory_free(m->operation_storage);
        solar_os_memory_free(m);
        xSemaphoreGive(registry_mutex);
        return ESP_ERR_NO_MEM;
    }
    strlcpy(m->info.name, registration->name, sizeof(m->info.name));
    strlcpy(m->info.driver, registration->driver, sizeof(m->info.driver));
    m->info.can_drive = registration->ops->drive != NULL;
    m->info.can_brake = registration->ops->brake != NULL;
    m->info.can_clear = registration->ops->clear != NULL;
    m->ops = *registration->ops;
    m->ctx = registration->ctx;
    motor_t **tail = &motors;
    while (*tail != NULL) tail = &(*tail)->next;
    *tail = m;
    xSemaphoreGive(registry_mutex);
    return ESP_OK;
}

esp_err_t solar_os_motor_unregister(const char *name)
{
    if (!valid_name(name, SOLAR_OS_MOTOR_NAME_MAX)) return ESP_ERR_INVALID_ARG;
    lock();
    motor_t *m = find(name);
    if (m == NULL) { xSemaphoreGive(registry_mutex); return ESP_ERR_NOT_FOUND; }
    if (m->refs != 0U || m->removing) { xSemaphoreGive(registry_mutex); return ESP_ERR_INVALID_STATE; }
    m->removing = true;
    xSemaphoreGive(registry_mutex);
    const esp_err_t ret = m->ops.shutdown != NULL ? m->ops.shutdown(m->ctx) : m->ops.coast(m->ctx);
    lock();
    if (ret == ESP_OK) {
        motor_t **link = &motors;
        while (*link != m) link = &(*link)->next;
        *link = m->next;
    } else m->removing = false;
    xSemaphoreGive(registry_mutex);
    if (ret == ESP_OK) {
        vSemaphoreDelete(m->operation_mutex);
        solar_os_memory_free(m->operation_storage);
        solar_os_memory_free(m);
    }
    return ret;
}

size_t solar_os_motor_count(void)
{
    lock();
    size_t count = 0;
    for (motor_t *m = motors; m != NULL; m = m->next) if (!m->removing) count++;
    xSemaphoreGive(registry_mutex);
    return count;
}

bool solar_os_motor_get(size_t index, solar_os_motor_info_t *info)
{
    if (info == NULL) return false;
    lock();
    motor_t *found = NULL;
    for (motor_t *m = motors; m != NULL; m = m->next) {
        if (!m->removing && index-- == 0U) { found = m; break; }
    }
    if (found != NULL) *info = found->info;
    xSemaphoreGive(registry_mutex);
    return found != NULL;
}

esp_err_t solar_os_motor_status(const char *name, solar_os_motor_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    motor_t *m;
    esp_err_t ret = acquire(name, &m);
    if (ret != ESP_OK) return ret;
    solar_os_motor_status_t value = {0};
    ret = m->ops.status(m->ctx, &value);
    if (ret == ESP_OK) { value.info = m->info; *status = value; }
    release(m);
    return ret;
}

esp_err_t solar_os_motor_drive(const char *name, float value)
{
    if (!isfinite(value) || value < -1.0f || value > 1.0f) return ESP_ERR_INVALID_ARG;
    motor_t *m;
    esp_err_t ret = acquire(name, &m);
    if (ret != ESP_OK) return ret;
    if (value == 0.0f) ret = m->ops.coast(m->ctx);
    else if (!m->info.can_drive) ret = ESP_ERR_NOT_SUPPORTED;
    else ret = m->ops.drive(m->ctx, value);
    release(m);
    return ret;
}

static esp_err_t simple_operation(const char *name, unsigned operation)
{
    motor_t *m;
    esp_err_t ret = acquire(name, &m);
    if (ret != ESP_OK) return ret;
    esp_err_t (*callback)(void *) = operation == 0U ? m->ops.coast : operation == 1U ? m->ops.brake : m->ops.clear;
    ret = callback != NULL ? callback(m->ctx) : ESP_ERR_NOT_SUPPORTED;
    release(m);
    return ret;
}

esp_err_t solar_os_motor_coast(const char *name) { return simple_operation(name, 0U); }
esp_err_t solar_os_motor_brake(const char *name) { return simple_operation(name, 1U); }
esp_err_t solar_os_motor_clear(const char *name) { return simple_operation(name, 2U); }

const char *solar_os_motor_mode_name(solar_os_motor_mode_t mode)
{
    switch (mode) {
    case SOLAR_OS_MOTOR_MODE_COAST: return "coast";
    case SOLAR_OS_MOTOR_MODE_FORWARD: return "forward";
    case SOLAR_OS_MOTOR_MODE_REVERSE: return "reverse";
    case SOLAR_OS_MOTOR_MODE_BRAKE: return "brake";
    default: return "unknown";
    }
}
