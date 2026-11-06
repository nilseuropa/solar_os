#include "solar_os_servo.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_pwm.h"
#include "solar_os_stream.h"
#include "esp_timer.h"

typedef struct {
    bool used;
    unsigned refs;
    SemaphoreHandle_t mutex;
    solar_os_servo_info_t info;
    char pwm_owner[SOLAR_OS_SERVO_OWNER_MAX];
    bool stream_registered, pwm_closed, input_revoked;
    uint64_t command_time_us;
} servo_slot_t;
static esp_err_t servo_publish(servo_slot_t *slot);
static esp_err_t servo_close_locked(servo_slot_t *slot);

/* Calibration and command state is task-only. Keep mutex storage internal. */
static EXT_RAM_BSS_ATTR servo_slot_t servos[SOLAR_OS_SERVO_MAX];
static StaticSemaphore_t servo_slot_mutex_storage[SOLAR_OS_SERVO_MAX];
static SemaphoreHandle_t servo_mutex;
static StaticSemaphore_t servo_mutex_storage;
static portMUX_TYPE servo_init_lock = portMUX_INITIALIZER_UNLOCKED;

static void servo_lock(void)
{
    portENTER_CRITICAL(&servo_init_lock);
    if (servo_mutex == NULL) {
        servo_mutex = xSemaphoreCreateMutexStatic(&servo_mutex_storage);
        for (size_t i = 0; i < SOLAR_OS_SERVO_MAX; i++) {
            servos[i].mutex = xSemaphoreCreateMutexStatic(&servo_slot_mutex_storage[i]);
        }
    }
    portEXIT_CRITICAL(&servo_init_lock);
    xSemaphoreTake(servo_mutex, portMAX_DELAY);
}

static servo_slot_t *servo_find_locked(const char *name)
{
    if (name == NULL) return NULL;
    for (size_t i = 0; i < SOLAR_OS_SERVO_MAX; i++) {
        if (servos[i].used && strcmp(servos[i].info.name, name) == 0) return &servos[i];
    }
    return NULL;
}

static bool valid_name(const char *name)
{
    if (name == NULL || name[0] == '\0' || strlen(name) >= SOLAR_OS_SERVO_NAME_MAX) return false;
    for (const char *p = name; *p != '\0'; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static void servo_release(servo_slot_t *slot)
{
    xSemaphoreGive(slot->mutex);
    servo_lock();
    slot->refs--;
    xSemaphoreGive(servo_mutex);
}

/* Called with the registry locked. References prevent slot reuse while a
 * caller waits; only this servo's operation mutex is held across backend I/O. */
static esp_err_t servo_take_locked(servo_slot_t *slot)
{
    slot->refs++;
    xSemaphoreGive(servo_mutex);
    xSemaphoreTake(slot->mutex, portMAX_DELAY);
    servo_lock();
    const bool used = slot->used;
    xSemaphoreGive(servo_mutex);
    if (!used) { servo_release(slot); return ESP_ERR_NOT_FOUND; }
    return ESP_OK;
}

static esp_err_t servo_acquire(const char *name, const char *owner, servo_slot_t **slot)
{
    if (name == NULL || (owner != NULL && owner[0] == '\0')) return ESP_ERR_INVALID_ARG;
    servo_lock();
    *slot = servo_find_locked(name);
    esp_err_t ret = *slot == NULL ? ESP_ERR_NOT_FOUND :
        owner != NULL && strcmp((*slot)->info.owner, owner) != 0 ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (ret != ESP_OK) { xSemaphoreGive(servo_mutex); return ret; }
    return servo_take_locked(*slot);
}

static esp_err_t servo_open_mode(const char *name, const char *owner,
                                 const solar_os_servo_config_t *config,bool writable)
{
    if (!valid_name(name) || owner == NULL || owner[0] == '\0' ||
        strlen(owner) >= SOLAR_OS_SERVO_OWNER_MAX || config == NULL ||
        config->freq_hz == 0U || config->freq_hz > SOLAR_OS_SERVO_FREQ_MAX_HZ ||
        config->pulse_min_us == 0U || config->pulse_min_us >= config->pulse_max_us ||
        (uint64_t)config->pulse_max_us * config->freq_hz >= 1000000U ||
        !isfinite(config->angle_min_rad) || !isfinite(config->angle_max_rad) ||
        config->angle_min_rad >= config->angle_max_rad ||
        !isfinite(config->angle_max_rad - config->angle_min_rad)) return ESP_ERR_INVALID_ARG;
    servo_lock();
    if (servo_find_locked(name) != NULL) {
        xSemaphoreGive(servo_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    servo_slot_t *slot = NULL;
    for (size_t i = 0; i < SOLAR_OS_SERVO_MAX; i++) {
        if (!servos[i].used && servos[i].refs == 0U) { slot = &servos[i]; break; }
    }
    if (slot == NULL) { xSemaphoreGive(servo_mutex); return ESP_ERR_NO_MEM; }
    /* An unused slot with no references cannot have an operation in flight. */
    xSemaphoreTake(slot->mutex, portMAX_DELAY);
    slot->info = (solar_os_servo_info_t){.config = *config,.stream_writable=writable};
    slot->stream_registered=false;slot->pwm_closed=false;slot->input_revoked=false;slot->command_time_us=0;
    memcpy(slot->info.name, name, strlen(name) + 1U);
    memcpy(slot->info.owner, owner, strlen(owner) + 1U);
    snprintf(slot->pwm_owner, sizeof(slot->pwm_owner), "servo:%s", name);
    slot->used = true;
    slot->refs = 1U;
    xSemaphoreGive(servo_mutex);
    esp_err_t ret = solar_os_pwm_open_output(&config->output, config->freq_hz, slot->pwm_owner);
    if (ret != ESP_OK) {
        servo_lock();
        slot->used = false;
        xSemaphoreGive(servo_mutex);
    } else {
        ret=servo_publish(slot);
        if(ret!=ESP_OK) (void)servo_close_locked(slot);
    }
    servo_release(slot);
    return ret;
}
esp_err_t solar_os_servo_open(const char *name,const char *owner,const solar_os_servo_config_t *config)
{ return servo_open_mode(name,owner,config,true); }
esp_err_t solar_os_servo_open_private(const char *name,const char *owner,const solar_os_servo_config_t *config)
{ return servo_open_mode(name,owner,config,false); }

size_t solar_os_servo_count(void)
{
    servo_lock();
    size_t count = 0;
    for (size_t i = 0; i < SOLAR_OS_SERVO_MAX; i++) count += servos[i].used;
    xSemaphoreGive(servo_mutex);
    return count;
}

bool solar_os_servo_get(size_t index, solar_os_servo_info_t *info)
{
    if (info == NULL) return false;
    servo_lock();
    servo_slot_t *slot = NULL;
    for (size_t i = 0; i < SOLAR_OS_SERVO_MAX; i++) {
        if (servos[i].used && index-- == 0U) { slot = &servos[i]; break; }
    }
    if (slot == NULL) { xSemaphoreGive(servo_mutex); return false; }
    if (servo_take_locked(slot) != ESP_OK) return false;
    *info = slot->info;
    servo_release(slot);
    return true;
}

esp_err_t solar_os_servo_status(const char *name, solar_os_servo_info_t *info)
{
    if (info == NULL) return ESP_ERR_INVALID_ARG;
    servo_slot_t *slot;
    const esp_err_t ret = servo_acquire(name, NULL, &slot);
    if (ret == ESP_OK) { *info = slot->info; servo_release(slot); }
    return ret;
}

static esp_err_t servo_pulse_locked(servo_slot_t *slot, uint32_t pulse_us,
                                     float angle_rad)
{
    const solar_os_servo_config_t *config = &slot->info.config;
    if (pulse_us < config->pulse_min_us || pulse_us > config->pulse_max_us) return ESP_ERR_INVALID_ARG;
    const esp_err_t ret = solar_os_pwm_write_output(&config->output, slot->pwm_owner, pulse_us);
    if (ret == ESP_OK) {
        slot->info.enabled = true;
        slot->info.has_command = true;
        slot->info.commanded_pulse_us = pulse_us;
        slot->info.commanded_angle_rad = angle_rad;
        slot->command_time_us=(uint64_t)esp_timer_get_time();
    }
    return ret;
}

static esp_err_t servo_set_locked(servo_slot_t *slot,float angle_rad)
{
    const solar_os_servo_config_t *c=&slot->info.config;
    if(!isfinite(angle_rad) || angle_rad<c->angle_min_rad || angle_rad>c->angle_max_rad) return ESP_ERR_INVALID_ARG;
    double ratio=((double)angle_rad-c->angle_min_rad)/((double)c->angle_max_rad-c->angle_min_rad);
    if(c->inverted) ratio=1.0-ratio;
    uint32_t pulse=c->pulse_min_us+(uint32_t)(ratio*(c->pulse_max_us-c->pulse_min_us)+.5);
    return servo_pulse_locked(slot,pulse,angle_rad);
}

esp_err_t solar_os_servo_set(const char *name, const char *owner, float angle_rad)
{
    if (!isfinite(angle_rad)) return ESP_ERR_INVALID_ARG;
    servo_slot_t *slot = NULL;
    if (owner == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = servo_acquire(name, owner, &slot);
    if (ret != ESP_OK) return ret;
    if (ret == ESP_OK) ret=slot->info.closing || slot->info.input_claimed?ESP_ERR_INVALID_STATE:servo_set_locked(slot,angle_rad);
    servo_release(slot);
    return ret;
}

esp_err_t solar_os_servo_pulse(const char *name, const char *owner, uint32_t pulse_us)
{
    servo_slot_t *slot = NULL;
    if (owner == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = servo_acquire(name, owner, &slot);
    if (ret != ESP_OK) return ret;
    if(slot->info.closing || slot->info.input_claimed) { servo_release(slot);return ESP_ERR_INVALID_STATE; }
    if (ret == ESP_OK) {
        const solar_os_servo_config_t *c = &slot->info.config;
        if (pulse_us < c->pulse_min_us || pulse_us > c->pulse_max_us) ret = ESP_ERR_INVALID_ARG;
        else {
            double ratio = (double)(pulse_us - c->pulse_min_us) / (c->pulse_max_us - c->pulse_min_us);
            if (c->inverted) ratio = 1.0 - ratio;
            ret = servo_pulse_locked(slot, pulse_us,
                (float)(c->angle_min_rad + ratio * ((double)c->angle_max_rad - c->angle_min_rad)));
        }
    }
    servo_release(slot);
    return ret;
}

esp_err_t solar_os_servo_disable(const char *name, const char *owner)
{
    servo_slot_t *slot = NULL;
    if (owner == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = servo_acquire(name, owner, &slot);
    if (ret != ESP_OK) return ret;
    slot->input_revoked=true;
    if(slot->pwm_closed) { servo_release(slot);return ESP_OK; }
    if (ret == ESP_OK) {
        ret = solar_os_pwm_disable_output(&slot->info.config.output, slot->pwm_owner);
        if (ret == ESP_OK) slot->info.enabled = false;
    }
    servo_release(slot);
    return ret;
}

static esp_err_t servo_close_locked(servo_slot_t *slot)
{
    slot->info.closing=true;slot->input_revoked=true;
    esp_err_t ret=ESP_OK;
    if(!slot->pwm_closed) {
        ret=solar_os_pwm_close_output(&slot->info.config.output,slot->pwm_owner);
        if(ret!=ESP_OK) return ret;
        slot->pwm_closed=true;slot->info.enabled=false;
    }
    if(slot->stream_registered) {
        char endpoint[SOLAR_OS_STREAM_ID_MAX];snprintf(endpoint,sizeof(endpoint),"servo.%s",slot->info.name);
        ret=solar_os_stream_unregister(endpoint);
        if(ret!=ESP_OK) return ret;
        slot->stream_registered=false;
    }
    if (ret == ESP_OK) {
        servo_lock();
        slot->used = false;
        xSemaphoreGive(servo_mutex);
    }
    return ret;
}

#include "solar_os_servo_stream.inc"

esp_err_t solar_os_servo_close(const char *name, const char *owner)
{
    servo_slot_t *slot = NULL;
    if (owner == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = servo_acquire(name, owner, &slot);
    if (ret != ESP_OK) return ret;
    if (ret == ESP_OK) ret = servo_close_locked(slot);
    servo_release(slot);
    return ret;
}

esp_err_t solar_os_servo_release_owner(const char *owner)
{
    if (owner == NULL || owner[0] == '\0') return ESP_ERR_INVALID_ARG;
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < SOLAR_OS_SERVO_MAX; i++) {
        servo_lock();
        servo_slot_t *slot = &servos[i];
        if (!slot->used || strcmp(slot->info.owner, owner) != 0) {
            xSemaphoreGive(servo_mutex);
            continue;
        }
        esp_err_t ret = servo_take_locked(slot);
        if (ret == ESP_OK) { ret = servo_close_locked(slot); servo_release(slot); }
        if (ret != ESP_ERR_NOT_FOUND && result == ESP_OK) result = ret;
    }
    return result;
}
