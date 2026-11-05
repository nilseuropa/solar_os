#include "solar_os_encoder.h"

#include <string.h>
#include <math.h>
#include <stdio.h>
#include <limits.h>
#include "solar_os_parameters.h"
#include "solar_os_stream.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_memory.h"

typedef struct { struct encoder *device; unsigned index; } encoder_parameter_t;
typedef struct encoder {
    struct encoder *next;
    solar_os_encoder_info_t info;
    solar_os_encoder_ops_t ops;
    void *ctx;
    size_t refs;
    bool removing;
    uint32_t claim;
    StaticSemaphore_t *operation_storage;
    SemaphoreHandle_t operation_mutex;
    solar_os_encoder_config_t config;
    solar_os_native_encoder_feedback_v1_t feedback;
    int64_t accumulated;
    uint32_t stream_claim;
    solar_os_parameter_registration_t parameters;
    encoder_parameter_t parameter_users[6];
    char endpoint[SOLAR_OS_STREAM_ID_MAX];
    bool stream_registered, shutdown_done, registration_failed, cleanup_busy;
} encoder_t;

static esp_err_t publish(encoder_t *e);
static esp_err_t unpublish(encoder_t *e);
static esp_err_t reap_failed(const char *name);

static encoder_t *encoders;
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

static encoder_t *find(const char *name)
{
    for (encoder_t *e = encoders; e != NULL; e = e->next)
        if (strcmp(e->info.name, name) == 0) return e;
    return NULL;
}

static esp_err_t acquire(const char *name, encoder_t **result)
{
    if (!valid_name(name, SOLAR_OS_ENCODER_NAME_MAX)) return ESP_ERR_INVALID_ARG;
    lock();
    encoder_t *e = find(name);
    esp_err_t ret = e == NULL ? ESP_ERR_NOT_FOUND : e->removing ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (ret == ESP_OK) e->refs++;
    xSemaphoreGive(registry_mutex);
    if (ret == ESP_OK) {
        xSemaphoreTake(e->operation_mutex, portMAX_DELAY);
        *result = e;
    }
    return ret;
}

static void release(encoder_t *e)
{
    xSemaphoreGive(e->operation_mutex);
    lock();
    e->refs--;
    xSemaphoreGive(registry_mutex);
}

esp_err_t solar_os_encoder_register(const solar_os_encoder_registration_t *registration)
{
    if (registration == NULL || !valid_name(registration->name, SOLAR_OS_ENCODER_NAME_MAX) ||
        !valid_name(registration->driver, SOLAR_OS_ENCODER_DRIVER_MAX) || registration->ops == NULL ||
        registration->ops->read == NULL || (registration->mode != SOLAR_OS_ENCODER_MODE_X1 &&
        registration->mode != SOLAR_OS_ENCODER_MODE_X2 && registration->mode != SOLAR_OS_ENCODER_MODE_X4))
        return ESP_ERR_INVALID_ARG;
    esp_err_t pending=reap_failed(registration->name);
    if(pending!=ESP_OK) return pending;
    lock();
    if (find(registration->name) != NULL) { xSemaphoreGive(registry_mutex); return ESP_ERR_INVALID_STATE; }
    encoder_t *e = solar_os_memory_calloc(1U, sizeof(*e), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "encoder.device");
    if (e == NULL) { xSemaphoreGive(registry_mutex); return ESP_ERR_NO_MEM; }
    e->operation_storage = solar_os_memory_calloc(1U, sizeof(*e->operation_storage),
        SOLAR_OS_MEMORY_INTERNAL_CRITICAL, "encoder.lock");
    if (e->operation_storage != NULL) e->operation_mutex = xSemaphoreCreateMutexStatic(e->operation_storage);
    if (e->operation_mutex == NULL) {
        solar_os_memory_free(e->operation_storage);
        solar_os_memory_free(e);
        xSemaphoreGive(registry_mutex);
        return ESP_ERR_NO_MEM;
    }
    strlcpy(e->info.name, registration->name, sizeof(e->info.name));
    strlcpy(e->info.driver, registration->driver, sizeof(e->info.driver));
    e->info.mode = registration->mode;
    e->info.can_reset = registration->ops->reset != NULL;
    e->ops = *registration->ops;
    e->ctx = registration->ctx;
    e->config=(solar_os_encoder_config_t){.gear_ratio=1,.polarity=1,.velocity_tau=.1f};
    snprintf(e->endpoint,sizeof(e->endpoint),"enc.%s",e->info.name);
    e->removing=true;
    encoder_t **tail = &encoders;
    while (*tail != NULL) tail = &(*tail)->next;
    *tail = e;
    xSemaphoreGive(registry_mutex);
    esp_err_t err=publish(e);
    lock(); e->removing=err!=ESP_OK; xSemaphoreGive(registry_mutex);
    if(err!=ESP_OK) {
        /* Failed attachment retains only metadata until in-flight discovery
         * callbacks finish. It never retains the driver's borrowed context. */
        char name[SOLAR_OS_ENCODER_NAME_MAX];strlcpy(name,e->info.name,sizeof(name));
        lock();e->registration_failed=true; e->ctx=NULL; e->ops=(solar_os_encoder_ops_t){0};xSemaphoreGive(registry_mutex);
        (void)reap_failed(name);
    }
    return err;
}

esp_err_t solar_os_encoder_unregister(const char *name)
{
    if (!valid_name(name, SOLAR_OS_ENCODER_NAME_MAX)) return ESP_ERR_INVALID_ARG;
    lock();
    encoder_t *e = find(name);
    if (e == NULL) { xSemaphoreGive(registry_mutex); return ESP_ERR_NOT_FOUND; }
    if (e->refs != 0U || e->cleanup_busy || (e->removing && !e->registration_failed && !e->shutdown_done) || e->claim != 0) { xSemaphoreGive(registry_mutex); return ESP_ERR_INVALID_STATE; }
    e->cleanup_busy=true;
    e->removing = true;
    xSemaphoreGive(registry_mutex);
    esp_err_t ret = !e->shutdown_done && e->ops.shutdown != NULL ? e->ops.shutdown(e->ctx) : ESP_OK;
    if(ret==ESP_OK) { e->shutdown_done=true; ret=unpublish(e); }
    lock();
    if (ret == ESP_OK) {
        encoder_t **link = &encoders;
        while (*link != e) link = &(*link)->next;
        *link = e->next;
    } else { e->cleanup_busy=false; if(!e->shutdown_done) e->removing = false; }
    xSemaphoreGive(registry_mutex);
    if (ret == ESP_OK) {
        vSemaphoreDelete(e->operation_mutex);
        solar_os_memory_free(e->operation_storage);
        solar_os_memory_free(e);
    }
    return ret;
}

size_t solar_os_encoder_count(void)
{
    lock();
    size_t count = 0;
    for (encoder_t *e = encoders; e != NULL; e = e->next) if (!e->removing) count++;
    xSemaphoreGive(registry_mutex);
    return count;
}

bool solar_os_encoder_get(size_t index, solar_os_encoder_info_t *info)
{
    if (info == NULL) return false;
    lock();
    encoder_t *found = NULL;
    for (encoder_t *e = encoders; e != NULL; e = e->next)
        if (!e->removing && index-- == 0U) { found = e; break; }
    if (found != NULL) *info = found->info;
    xSemaphoreGive(registry_mutex);
    return found != NULL;
}

esp_err_t solar_os_encoder_status(const char *name, solar_os_encoder_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    encoder_t *e;
    esp_err_t ret = acquire(name, &e);
    if (ret != ESP_OK) return ret;
    solar_os_encoder_status_t value = {.info = e->info,.config=e->config,.feedback=e->feedback,.claimed=e->claim!=0};
    ret = e->ops.read(e->ctx, &value.count);
    if (ret == ESP_OK) *status = value;
    release(e);
    return ret;
}

esp_err_t solar_os_encoder_read(const char *name, int32_t *count)
{
    if (count == NULL) return ESP_ERR_INVALID_ARG;
    solar_os_encoder_status_t status;
    const esp_err_t ret = solar_os_encoder_status(name, &status);
    if (ret == ESP_OK) *count = status.count;
    return ret;
}

esp_err_t solar_os_encoder_reset(const char *name)
{
    encoder_t *e;
    esp_err_t ret = acquire(name, &e);
    if (ret != ESP_OK) return ret;
    ret = e->claim ? ESP_ERR_INVALID_STATE : e->ops.reset != NULL ? e->ops.reset(e->ctx) : ESP_ERR_NOT_SUPPORTED;
    if(ret==ESP_OK) { e->feedback=(solar_os_native_encoder_feedback_v1_t){0}; e->accumulated=0; }
    release(e);
    return ret;
}

esp_err_t solar_os_encoder_claim(const char *name, uint32_t *claim)
{
    if (claim == NULL) return ESP_ERR_INVALID_ARG;
    *claim = 0;
    encoder_t *e; esp_err_t ret = acquire(name, &e);
    if (ret != ESP_OK) return ret;
    lock();
    if (e->claim || e->refs != 1 || next_claim == UINT32_MAX) ret = ESP_ERR_INVALID_STATE;
    else { e->claim = ++next_claim; *claim = e->claim; }
    xSemaphoreGive(registry_mutex);
    release(e); return ret;
}

static esp_err_t acquire_owned(uint32_t claim, encoder_t **result)
{
    if (!claim) return ESP_ERR_INVALID_ARG;
    lock(); encoder_t *e;
    for (e = encoders; e != NULL && e->claim != claim; e = e->next) {}
    if (e != NULL) e->refs++;
    xSemaphoreGive(registry_mutex);
    if (e == NULL) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(e->operation_mutex, portMAX_DELAY);
    if (e->claim != claim) { release(e); return ESP_ERR_NOT_FOUND; }
    *result = e; return ESP_OK;
}

esp_err_t solar_os_encoder_sample_owned(uint32_t claim, solar_os_encoder_sample_t *sample)
{
    if (sample == NULL) return ESP_ERR_INVALID_ARG;
    encoder_t *e; esp_err_t ret = acquire_owned(claim, &e);
    if (ret != ESP_OK) return ret;
    solar_os_encoder_sample_t value = {.decoding = e->info.mode};
    ret = e->ops.read(e->ctx, &value.count);
    if (ret == ESP_OK) { value.timestamp_us = (uint64_t)esp_timer_get_time(); *sample = value; }
    release(e); return ret;
}

esp_err_t solar_os_encoder_release(uint32_t claim)
{
    encoder_t *e; esp_err_t ret = acquire_owned(claim, &e);
    if (ret != ESP_OK) return ret;
    lock(); e->claim = 0;e->stream_claim=0;e->feedback.valid=0; xSemaphoreGive(registry_mutex);
    release(e); return ESP_OK;
}

#include "solar_os_encoder_endpoints.inc"
