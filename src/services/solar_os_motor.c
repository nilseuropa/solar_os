#include "solar_os_motor.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include "solar_os_parameters.h"
#include "solar_os_stream.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_memory.h"

typedef struct { struct motor *device; unsigned index; } motor_parameter_t;
typedef struct motor {
    struct motor *next;
    solar_os_motor_info_t info;
    solar_os_motor_ops_t ops;
    void *ctx;
    size_t refs;
    bool removing;
    uint32_t claim;
    StaticSemaphore_t *operation_storage;
    SemaphoreHandle_t operation_mutex;
    solar_os_motor_config_t config;
    solar_os_parameter_registration_t parameters;
    motor_parameter_t parameter_users[3];
    char endpoint[SOLAR_OS_STREAM_ID_MAX];
    bool stream_registered, shutdown_done, registration_failed, cleanup_busy;
} motor_t;
static esp_err_t publish(motor_t *m);
static esp_err_t unpublish(motor_t *m);
static esp_err_t reap_failed(const char *name);
static esp_err_t drive_guarded(motor_t *m,float value,uint64_t valid_until_us);
static esp_err_t drive_locked(motor_t *m,float value) { return drive_guarded(m,value,0); }

static motor_t *motors;
static uint32_t next_claim;
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
    esp_err_t pending=reap_failed(registration->name);
    if(pending!=ESP_OK) return pending;
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
    m->config=(solar_os_motor_config_t){.polarity=1,.max_drive=1};
    snprintf(m->endpoint,sizeof(m->endpoint),"mot.%s",m->info.name);
    m->removing=true;
    motor_t **tail = &motors;
    while (*tail != NULL) tail = &(*tail)->next;
    *tail = m;
    xSemaphoreGive(registry_mutex);
    esp_err_t err=publish(m);
    lock();m->removing=err!=ESP_OK;xSemaphoreGive(registry_mutex);
    if(err!=ESP_OK) {
        char name[SOLAR_OS_MOTOR_NAME_MAX];strlcpy(name,m->info.name,sizeof(name));
        lock();m->registration_failed=true;m->ctx=NULL;m->ops=(solar_os_motor_ops_t){0};xSemaphoreGive(registry_mutex);
        (void)reap_failed(name);
    }
    return err;
}

esp_err_t solar_os_motor_unregister(const char *name)
{
    if (!valid_name(name, SOLAR_OS_MOTOR_NAME_MAX)) return ESP_ERR_INVALID_ARG;
    lock();
    motor_t *m = find(name);
    if (m == NULL) { xSemaphoreGive(registry_mutex); return ESP_ERR_NOT_FOUND; }
    if (m->refs != 0U || m->cleanup_busy || (m->removing && !m->registration_failed && !m->shutdown_done) || m->claim != 0U) { xSemaphoreGive(registry_mutex); return ESP_ERR_INVALID_STATE; }
    m->cleanup_busy=true;
    m->removing = true;
    xSemaphoreGive(registry_mutex);
    esp_err_t ret = m->shutdown_done || m->registration_failed ? ESP_OK :
        m->ops.shutdown != NULL ? m->ops.shutdown(m->ctx) : m->ops.coast(m->ctx);
    if(ret==ESP_OK) { m->shutdown_done=true;ret=unpublish(m); }
    lock();
    if (ret == ESP_OK) {
        motor_t **link = &motors;
        while (*link != m) link = &(*link)->next;
        *link = m->next;
    } else { m->cleanup_busy=false;if(!m->shutdown_done) m->removing = false; }
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
    if (ret == ESP_OK) { value.info = m->info; value.config=m->config; value.claimed=m->claim!=0; *status = value; }
    release(m);
    return ret;
}

esp_err_t solar_os_motor_drive(const char *name, float value)
{
    if (!isfinite(value) || value < -1.0f || value > 1.0f) return ESP_ERR_INVALID_ARG;
    motor_t *m;
    esp_err_t ret = acquire(name, &m);
    if (ret != ESP_OK) return ret;
    if (m->claim != 0) ret = ESP_ERR_INVALID_STATE;
    else ret=drive_locked(m,value);
    release(m);
    return ret;
}

static esp_err_t simple_operation(const char *name, unsigned operation)
{
    motor_t *m;
    esp_err_t ret = acquire(name, &m);
    if (ret != ESP_OK) return ret;
    esp_err_t (*callback)(void *) = operation == 0U ? m->ops.coast : operation == 1U ? m->ops.brake : m->ops.clear;
    ret = m->claim != 0 ? ESP_ERR_INVALID_STATE : callback != NULL ? callback(m->ctx) : ESP_ERR_NOT_SUPPORTED;
    release(m);
    return ret;
}

esp_err_t solar_os_motor_coast(const char *name) { return simple_operation(name, 0U); }
esp_err_t solar_os_motor_brake(const char *name) { return simple_operation(name, 1U); }
esp_err_t solar_os_motor_clear(const char *name) { return simple_operation(name, 2U); }

esp_err_t solar_os_motor_claim(const char *name, uint32_t *claim)
{
    if (claim == NULL) return ESP_ERR_INVALID_ARG;
    *claim = 0;
    motor_t *m;
    esp_err_t ret = acquire(name, &m);
    if (ret != ESP_OK) return ret;
    lock();
    if (m->claim || m->refs != 1 || next_claim == UINT32_MAX) ret = ESP_ERR_INVALID_STATE;
    else { m->claim = ++next_claim; *claim = m->claim; }
    xSemaphoreGive(registry_mutex);
    release(m);
    return ret;
}

static esp_err_t acquire_owned(uint32_t claim, motor_t **result)
{
    if (!claim) return ESP_ERR_INVALID_ARG;
    lock(); motor_t *m;
    for (m = motors; m != NULL && m->claim != claim; m = m->next) {}
    if (m != NULL) m->refs++;
    xSemaphoreGive(registry_mutex);
    if (m == NULL) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(m->operation_mutex, portMAX_DELAY);
    if (m->claim != claim) { release(m); return ESP_ERR_NOT_FOUND; }
    *result = m;
    return ESP_OK;
}

esp_err_t solar_os_motor_drive_owned(uint32_t claim, float value)
{
    if (!isfinite(value) || value < -1 || value > 1) return ESP_ERR_INVALID_ARG;
    motor_t *m; esp_err_t ret = acquire_owned(claim, &m);
    if (ret != ESP_OK) return ret;
    ret=drive_locked(m,value);
    release(m); return ret;
}

static esp_err_t stop_owned(uint32_t claim, bool brake, bool drop)
{
    motor_t *m; esp_err_t ret = acquire_owned(claim, &m);
    if (ret != ESP_OK) return ret;
    esp_err_t (*callback)(void *) = brake ? m->ops.brake : m->ops.coast;
    ret = callback != NULL ? callback(m->ctx) : ESP_ERR_NOT_SUPPORTED;
    if (ret == ESP_OK && drop) { lock(); m->claim = 0; xSemaphoreGive(registry_mutex); }
    release(m); return ret;
}
esp_err_t solar_os_motor_stop_owned(uint32_t claim, bool brake) { return stop_owned(claim, brake, false); }
esp_err_t solar_os_motor_release(uint32_t claim, bool brake) { return stop_owned(claim, brake, true); }

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

#include "solar_os_motor_endpoints.inc"
