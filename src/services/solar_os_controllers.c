#include "solar_os_controllers.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_pwm.h"
#include "solar_os_time.h"
#include "esp_timer.h"
#include "solar_os_encoder.h"
#include "solar_os_motor.h"

typedef struct {
    bool active;
    solar_os_controller_info_t info;
    uint64_t last_step_ms;
    bool clock_started;
    bool source_open;
    solar_os_stream_handle_t source_handle;
    solar_os_parameter_registration_t registration;
    uint32_t owner_id;
    uint64_t sample_us;
    solar_os_controller_tuner_t tuner;
    bool tune_manual;
    bool tune_has_sample;
    uint64_t tune_sample_ms;
    solar_os_stream_handle_t target_handle;
    bool source_persistent, target_open, output_claimed, managed, restore_encoder, restore_motor;
    solar_os_encoder_config_t saved_encoder;
    solar_os_motor_config_t saved_motor;
    solar_os_native_bound_controller_config_v1_t binding;
    uint16_t parameter_claims;
} controller_slot_t;

static EXT_RAM_BSS_ATTR controller_slot_t slots[SOLAR_OS_CONTROLLER_MAX];
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static uint32_t next_owner_id;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;

static void lock(void)
{
    portENTER_CRITICAL(&init_lock);
    if (mutex == NULL) mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    portEXIT_CRITICAL(&init_lock);
    xSemaphoreTake(mutex, portMAX_DELAY);
}

static controller_slot_t *find(const char *name)
{
    if (name != NULL) for (size_t i = 0; i < SOLAR_OS_CONTROLLER_MAX; i++)
        if (slots[i].active && strcmp(name, slots[i].info.config.name) == 0) return &slots[i];
    return NULL;
}

static bool tuning_running(const controller_slot_t *slot)
{ return slot->info.tuning.state==SOLAR_OS_CONTROLLER_TUNE_RUNNING; }
static esp_err_t stop_locked(controller_slot_t *slot);
static esp_err_t restore_binding(controller_slot_t *slot);
static void tick_locked(controller_slot_t *slot,uint64_t now_ms);
static esp_err_t command_locked(controller_slot_t *slot,float value)
{
    const solar_os_controller_config_t *c=&slot->info.config;
    if(!isfinite(value) || value<c->setpoint_min || value>c->setpoint_max) return ESP_ERR_INVALID_ARG;
    if(fabsf(value)<=c->velocity_deadband) value=0;
    slot->info.config.setpoint=value;slot->info.command_valid=true;
    slot->info.command_expires_us=c->command_timeout_ms?
        (uint64_t)esp_timer_get_time()+(uint64_t)c->command_timeout_ms*1000:0;
    slot->info.waiting=false;slot->info.last_error=ESP_OK;
    return ESP_OK;
}
static void expire_locked(controller_slot_t *slot)
{
    slot->info.command_valid=false;slot->info.command_expires_us=0;
    slot->info.config.setpoint=slot->info.active_setpoint=slot->info.feedforward=0;
    slot->info.waiting=true;slot->info.last_error=ESP_ERR_TIMEOUT;
    solar_os_controller_math_reset(&slot->info.config.algorithm,&slot->info.state,slot->info.config.safe_output);
}

static esp_err_t tuning_step(controller_slot_t *slot, float measurement, float dt, float *output)
{
    slot->info.feedforward=0;
    float relay=slot->info.config.safe_output;
    bool ok=solar_os_controller_tuner_step(&slot->tuner,measurement,dt,&relay);
    slot->info.tuning=slot->tuner.result;
    if(!ok) {
        slot->info.state.output=*output=slot->info.config.safe_output;
        return slot->info.tuning.reason==SOLAR_OS_CONTROLLER_TUNE_TIMEOUT?ESP_ERR_TIMEOUT:ESP_ERR_INVALID_RESPONSE;
    }
    *output=slot->info.tuning.state==SOLAR_OS_CONTROLLER_TUNE_COMPLETE?slot->info.config.safe_output:relay;
    slot->info.state.output=*output;
    slot->info.measurement=measurement;
    slot->info.steps++;
    return ESP_OK;
}

static bool name_valid(const char *name)
{
    if (name == NULL || *name == '\0' || strnlen(name, SOLAR_OS_CONTROLLER_NAME_MAX) >=
        SOLAR_OS_CONTROLLER_NAME_MAX) return false;
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    return true;
}

static esp_err_t setpoint_get(void *user, float *value)
{
    lock(); *value = ((controller_slot_t *)user)->info.config.setpoint;
    xSemaphoreGive(mutex); return ESP_OK;
}

static esp_err_t setpoint_set(void *user, float value)
{
    if (!isfinite(value)) return ESP_ERR_INVALID_ARG;
    lock(); controller_slot_t *slot=user;
    esp_err_t err=slot->owner_id || tuning_running(slot)?ESP_ERR_INVALID_STATE:ESP_OK;
    if(err==ESP_OK) err=command_locked(slot,value);
    xSemaphoreGive(mutex); return err;
}

static void writer_name(const controller_slot_t *slot, char *text, size_t size)
{ snprintf(text, size, "ctl.%s", slot->info.config.name); }

static bool pwm_target(const controller_slot_t *slot, solar_os_pwm_output_t *output)
{
    return strncmp(slot->info.config.target, "pwm:", 4) == 0 &&
        solar_os_pwm_parse_output(slot->info.config.target + 4, output);
}

static esp_err_t write_output(controller_slot_t *slot, float value)
{
    char writer[SOLAR_OS_PARAMETER_WRITER_MAX];
    writer_name(slot, writer, sizeof(writer));
    if (strncmp(slot->info.config.target, "parameter:", 10) == 0)
        return solar_os_parameter_set_owned(slot->info.config.target + 10, writer, value);
    if(slot->target_open) {
        size_t written=0;
        solar_os_stream_scalar_command_t command={.value=value};
        const void *data=&value;size_t bytes=sizeof(value);
        if(!strncmp(slot->info.config.target,"stream:mot.",11)) {
            uint64_t feedback_deadline=(slot->info.sample_ms+slot->info.config.timeout_ms)*1000;
            uint64_t gap_deadline=slot->last_step_ms*1000+(uint64_t)(slot->info.config.algorithm.max_dt*1000000);
            command.valid_until_us=feedback_deadline<gap_deadline?feedback_deadline:gap_deadline;
            if(slot->info.command_valid && slot->info.command_expires_us && !tuning_running(slot) &&
                slot->info.command_expires_us<command.valid_until_us) command.valid_until_us=slot->info.command_expires_us;
            data=&command;bytes=sizeof(command);
        }
        esp_err_t err=solar_os_stream_write(&slot->target_handle,data,bytes,slot->info.config.period_ms,&written);
        return err==ESP_OK && written!=bytes?ESP_ERR_INVALID_RESPONSE:err;
    }
    solar_os_pwm_output_t output;
    if (!pwm_target(slot, &output)) return ESP_ERR_INVALID_ARG;
    return solar_os_pwm_write_output(&output, writer, (uint32_t)lroundf(value));
}

static esp_err_t release_output(controller_slot_t *slot)
{
    char writer[SOLAR_OS_PARAMETER_WRITER_MAX];
    writer_name(slot, writer, sizeof(writer));
    esp_err_t err;
    if (strncmp(slot->info.config.target, "parameter:", 10) == 0) {
        err = solar_os_parameter_release(slot->info.config.target + 10, writer);
        /* Unregister invalidates the claim, and owned writes cannot reach a replacement. */
        if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;
    } else if(slot->target_open) {
        err=solar_os_stream_close_ex(&slot->target_handle);
        if(err==ESP_OK) slot->target_open=false;
    } else {
        solar_os_pwm_output_t output;
        err = pwm_target(slot, &output) ? solar_os_pwm_close_output(&output, writer) : ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK) slot->output_claimed=false;
    slot->info.claimed=slot->output_claimed || slot->source_open || slot->parameter_claims;
    return err;
}

esp_err_t solar_os_controller_create(const char *name, solar_os_controller_kind_t kind)
{
    if (!name_valid(name) || strlen(name)+sizeof(".setpoint")>SOLAR_OS_PARAMETER_NAME_MAX) return ESP_ERR_INVALID_ARG;
    solar_os_controller_math_config_t algorithm = solar_os_controller_math_defaults(kind);
    if (!solar_os_controller_math_valid(&algorithm)) return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t err = find(name) != NULL ? ESP_ERR_INVALID_STATE : ESP_ERR_NO_MEM;
    if (err == ESP_ERR_NO_MEM) for (size_t i = 0; i < SOLAR_OS_CONTROLLER_MAX; i++) {
        controller_slot_t *slot = &slots[i];
        if (slot->active) continue;
        *slot = (controller_slot_t){0};
        slot->source_handle = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
        slot->target_handle = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
        strlcpy(slot->info.config.name, name, sizeof(slot->info.config.name));
        slot->info.config.algorithm = algorithm;
        slot->info.config.setpoint_min = -1e20f;
        slot->info.config.setpoint_max = 1e20f;
        slot->info.config.period_ms = 20;
        slot->info.config.timeout_ms = 200;
        slot->info.config.frequency_hz = 50;
        solar_os_controller_math_reset(&algorithm, &slot->info.state, 0);
        char parameter[SOLAR_OS_CONTROLLER_NAME_MAX+sizeof(".setpoint")];
        snprintf(parameter, sizeof(parameter), "%s.setpoint", name);
        const solar_os_parameter_definition_t definition = {
            .name = parameter, .label = "Setpoint", .unit = "", .minimum = -1e20f,
            .maximum = 1e20f, .get = setpoint_get, .set = setpoint_set, .user = slot,
        };
        err = solar_os_parameters_register("controller", &definition, 1, &slot->registration);
        if (err == ESP_OK) slot->active = true;
        break;
    }
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_controller_delete(const char *name)
{
    lock(); controller_slot_t *slot = find(name);
    esp_err_t err = slot == NULL ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    if (slot != NULL && !slot->owner_id && !tuning_running(slot) && !slot->info.enabled && !slot->info.claimed) {
        err = solar_os_parameters_unregister(&slot->registration);
        if (err == ESP_OK) *slot = (controller_slot_t){0};
    }
    xSemaphoreGive(mutex); return err;
}

size_t solar_os_controller_count(void)
{
    lock(); size_t count = 0;
    for (size_t i = 0; i < SOLAR_OS_CONTROLLER_MAX; i++) count += slots[i].active;
    xSemaphoreGive(mutex); return count;
}

bool solar_os_controller_get_info(size_t index, solar_os_controller_info_t *info)
{
    if (info == NULL) return false;
    lock(); bool found = false;
    for (size_t i = 0; i < SOLAR_OS_CONTROLLER_MAX; i++) if (slots[i].active && index-- == 0) {
        *info = slots[i].info; found = true; break;
    }
    xSemaphoreGive(mutex); return found;
}

esp_err_t solar_os_controller_find(const char *name, solar_os_controller_info_t *info)
{
    if (info == NULL) return ESP_ERR_INVALID_ARG;
    lock(); controller_slot_t *slot = find(name);
    if (slot != NULL) *info = slot->info;
    xSemaphoreGive(mutex); return slot != NULL ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_controller_configure(const char *name, const char *key, float value)
{
    if (key == NULL || !isfinite(value)) return ESP_ERR_INVALID_ARG;
    /* Route setpoint writes through the same claim policy as controls/OSC. */
    if (strcmp(key, "setpoint") == 0) {
        if (!name_valid(name)) return ESP_ERR_INVALID_ARG;
        lock(); controller_slot_t *slot=find(name);
        bool owned=slot && (slot->owner_id || tuning_running(slot));
        xSemaphoreGive(mutex);
        if(owned) return ESP_ERR_INVALID_STATE;
        char path[SOLAR_OS_PARAMETER_PATH_MAX];
        snprintf(path, sizeof(path), "controller.%s.setpoint", name);
        return solar_os_parameter_set(path, value);
    }
    lock(); controller_slot_t *slot = find(name);
    esp_err_t err = slot == NULL ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    if (slot != NULL && !slot->owner_id && !tuning_running(slot) && !slot->info.enabled && !slot->info.claimed) {
        solar_os_controller_config_t c = slot->info.config;
        err = ESP_OK;
        if (strcmp(key, "kp") == 0) c.algorithm.kp = value;
        else if (strcmp(key, "ki") == 0) c.algorithm.ki = value;
        else if (strcmp(key, "kd") == 0) c.algorithm.kd = value;
        else if (strcmp(key, "tau") == 0) c.algorithm.derivative_tau = value;
        else if (strcmp(key, "output_min") == 0) c.algorithm.output_min = value;
        else if (strcmp(key, "output_max") == 0) c.algorithm.output_max = value;
        else if (strcmp(key, "hysteresis") == 0) c.algorithm.hysteresis = value;
        else if (strcmp(key, "max_dt") == 0) c.algorithm.max_dt = value;
        else if (strcmp(key, "reverse") == 0 && (value == 0 || value == 1)) c.algorithm.reverse = value != 0;
        else if (strcmp(key, "safe_output") == 0) c.safe_output = value;
        else if (strcmp(key, "setpoint_min") == 0) c.setpoint_min = value;
        else if (strcmp(key, "setpoint_max") == 0) c.setpoint_max = value;
        else if (strcmp(key, "acceleration_limit") == 0 && value>=0) c.acceleration_limit=value;
        else if (strcmp(key, "velocity_deadband") == 0 && value>=0) c.velocity_deadband=value;
        else if (strcmp(key, "friction_bias_forward") == 0 && value>=0) c.friction_bias_forward=value;
        else if (strcmp(key, "friction_bias_reverse") == 0 && value>=0) c.friction_bias_reverse=value;
        else if (strcmp(key, "command_timeout_ms") == 0 && value>=0 && value<=60000 && floorf(value)==value) c.command_timeout_ms=(uint32_t)value;
        else if (value >= 1 && value <= 60000 && floorf(value) == value) {
            if (strcmp(key, "period_ms") == 0 && value >= 10) c.period_ms = (uint32_t)value;
            else if (strcmp(key, "timeout_ms") == 0) c.timeout_ms = (uint32_t)value;
            else if (strcmp(key, "frequency_hz") == 0 && value <= SOLAR_OS_PWM_FREQ_MAX_HZ) c.frequency_hz = (uint32_t)value;
            else err = ESP_ERR_INVALID_ARG;
        } else err = ESP_ERR_INVALID_ARG;
        if (!solar_os_controller_math_valid(&c.algorithm)) err = ESP_ERR_INVALID_ARG;
        if (c.setpoint_min >= c.setpoint_max || c.setpoint < c.setpoint_min ||
            c.setpoint > c.setpoint_max) err = ESP_ERR_INVALID_ARG;
        if(c.friction_bias_forward>c.algorithm.output_max || -c.friction_bias_reverse<c.algorithm.output_min ||
            (c.command_timeout_ms && c.command_timeout_ms<c.period_ms)) err=ESP_ERR_INVALID_ARG;
        if (err == ESP_OK && (strcmp(key, "setpoint_min") == 0 || strcmp(key, "setpoint_max") == 0)) {
            char parameter[SOLAR_OS_CONTROLLER_NAME_MAX+sizeof(".setpoint")];
            snprintf(parameter, sizeof(parameter), "%s.setpoint", c.name);
            err = solar_os_parameters_update_range(&slot->registration, parameter, c.setpoint_min, c.setpoint_max);
        }
        if (err == ESP_OK) {
            slot->info.config = c;
            float initial = fminf(fmaxf(slot->info.state.output, c.algorithm.output_min), c.algorithm.output_max);
            solar_os_controller_math_reset(&c.algorithm, &slot->info.state, initial);
        }
    }
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_controller_bind(const char *name, const char *source, const char *target)
{
    if (source == NULL || target == NULL || strlen(source) >= SOLAR_OS_STREAM_ID_MAX ||
        strlen(target) >= SOLAR_OS_CONTROLLER_TARGET_MAX ||
        (strncmp(target, "parameter:", 10) != 0 && strncmp(target, "pwm:", 4) != 0 && strncmp(target,"stream:",7)!=0) ||
        strncmp(target, "parameter:controller.", 21) == 0) return ESP_ERR_INVALID_ARG;
    lock(); controller_slot_t *slot = find(name);
    esp_err_t err = slot == NULL ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    if (slot != NULL && !slot->owner_id && !tuning_running(slot) && !slot->info.enabled && !slot->info.claimed) {
        strlcpy(slot->info.config.source, strcmp(source, "manual") == 0 ? "" : source, sizeof(slot->info.config.source));
        strlcpy(slot->info.config.target, target, sizeof(slot->info.config.target));
        slot->info.has_measurement = false;
        err = ESP_OK;
    }
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_controller_feed(const char *name, float measurement, uint64_t sample_ms)
{
    if (!isfinite(measurement) || sample_ms > solar_os_time_uptime_ms()) return ESP_ERR_INVALID_ARG;
    lock(); controller_slot_t *slot = find(name);
    esp_err_t err = slot == NULL ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    if (slot != NULL && !slot->owner_id && slot->info.config.source[0] == '\0') {
        if (slot->info.has_measurement && sample_ms < slot->info.sample_ms) err = ESP_ERR_INVALID_ARG;
        else {
            slot->info.measurement = measurement; slot->info.sample_ms = sample_ms;
            slot->info.has_measurement = true; err = ESP_OK;
        }
    }
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_controller_step(const char *name, float measurement, float dt, float *output)
{
    if (output == NULL) return ESP_ERR_INVALID_ARG;
    lock(); controller_slot_t *slot = find(name);
    esp_err_t err = slot == NULL ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    if (slot != NULL && !slot->owner_id && !slot->info.enabled && !slot->info.claimed) {
        if(tuning_running(slot) && slot->tune_manual) {
            err=tuning_step(slot,measurement,dt,output);
            slot->info.last_error=err;
            if(err!=ESP_OK) { slot->info.faulted=true; slot->info.faults++; }
            xSemaphoreGive(mutex); return err;
        }
        if(slot->tune_manual) {
            *output=slot->info.config.safe_output;
            xSemaphoreGive(mutex); return ESP_ERR_INVALID_STATE;
        }
        err = solar_os_controller_math_step(&slot->info.config.algorithm, &slot->info.state,
            slot->info.config.setpoint, measurement, dt) ? ESP_OK : ESP_ERR_INVALID_ARG;
        if (err == ESP_OK) {
            *output = slot->info.state.output; slot->info.measurement = measurement;
            slot->info.steps++;
        }
    }
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_controller_manual(const char *name, float output)
{
    lock(); controller_slot_t *slot = find(name);
    esp_err_t err = slot == NULL ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_ARG;
    if(slot && (slot->owner_id || tuning_running(slot))) err=ESP_ERR_INVALID_STATE;
    else if (slot != NULL && solar_os_controller_math_manual(&slot->info.config.algorithm, &slot->info.state, output)) err = ESP_OK;
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_controller_auto(const char *name)
{
    lock(); controller_slot_t *slot = find(name);
    esp_err_t err=slot?(slot->owner_id || tuning_running(slot))?ESP_ERR_INVALID_STATE:ESP_OK:ESP_ERR_NOT_FOUND;
    if (err==ESP_OK) solar_os_controller_math_auto(&slot->info.state);
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_controller_reset(const char *name)
{
    lock(); controller_slot_t *slot = find(name);
    esp_err_t err = slot == NULL ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    if (slot != NULL && !slot->owner_id && !tuning_running(slot) && !slot->info.enabled && !slot->info.claimed) {
        err = solar_os_controller_math_reset(&slot->info.config.algorithm, &slot->info.state,
            slot->info.config.safe_output) ? ESP_OK : ESP_ERR_INVALID_ARG;
        if (err == ESP_OK) {
            slot->info.faulted = false; slot->info.last_error = ESP_OK;
            slot->info.has_measurement = false;
            slot->info.feedforward=slot->info.active_setpoint=0;
            memset(&slot->info.tuning,0,sizeof(slot->info.tuning));
            slot->tune_manual=false;
        }
    }
    xSemaphoreGive(mutex); return err;
}

static esp_err_t start_locked(controller_slot_t *slot)
{
    const solar_os_controller_config_t *c = &slot->info.config;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (c->safe_output >= c->algorithm.output_min && c->safe_output <= c->algorithm.output_max &&
        c->algorithm.max_dt * 1000 >= c->period_ms && c->timeout_ms >= c->period_ms) {
        char writer[SOLAR_OS_PARAMETER_WRITER_MAX]; writer_name(slot, writer, sizeof(writer));
        if (strncmp(c->target, "parameter:", 10) == 0) {
            solar_os_parameter_info_t info;
            err = solar_os_parameter_find(c->target + 10, &info);
            if (err == ESP_OK && (c->algorithm.output_min < info.minimum ||
                c->algorithm.output_max > info.maximum)) err = ESP_ERR_INVALID_ARG;
            if (err == ESP_OK) err = solar_os_parameter_claim(c->target + 10, writer);
        } else if(!strncmp(c->target,"stream:",7)) {
            solar_os_stream_info_t info;
            err=solar_os_stream_get_info(c->target+7,&info);
            if(err==ESP_OK && (info.type!=SOLAR_OS_STREAM_TYPE_SCALAR ||
                (info.direction!=SOLAR_OS_STREAM_DIRECTION_SINK && info.direction!=SOLAR_OS_STREAM_DIRECTION_DUPLEX) ||
                strcmp(info.format,"f32"))) err=ESP_ERR_INVALID_ARG;
            if(err==ESP_OK && !strncmp(c->target+7,"mot.",4) && !slot->managed) {
                solar_os_motor_config_t config;
                err=solar_os_motor_config_get(c->target+11,&config);
                if(err==ESP_OK && (c->safe_output!=0 || c->algorithm.output_min < -config.max_drive ||
                    c->algorithm.output_max>config.max_drive)) err=ESP_ERR_INVALID_ARG;
            }
            if(err==ESP_OK) {
                const solar_os_stream_open_options_t options={.direction=SOLAR_OS_STREAM_DIRECTION_SINK};
                err=solar_os_stream_open_ex(c->target+7,c->name,&options,&slot->target_handle);
                if(err==ESP_OK) slot->target_open=true;
            }
        } else {
            solar_os_pwm_output_t output;
            if (pwm_target(slot, &output) && c->algorithm.output_min >= 0 &&
                (double)lroundf(c->algorithm.output_max) * c->frequency_hz < 1000000.0)
                err = solar_os_pwm_open_output(&output, c->frequency_hz, writer);
        }
    }
    if (err == ESP_OK) {
        slot->info.enabled = slot->info.claimed = true; slot->info.faulted = false;
        slot->output_claimed=true;
        slot->source_persistent=!strncmp(c->source,"enc.",4);
        if(slot->source_persistent) {
            err=solar_os_stream_open(c->source,c->name,&slot->source_handle);
            if(err==ESP_OK) slot->source_open=true;
            if(err==ESP_OK && !slot->managed) {
                solar_os_encoder_config_t config;
                err=solar_os_encoder_config_get(c->source+4,&config);
                if(err==ESP_OK && config.ppr<=0) err=ESP_ERR_INVALID_STATE;
            }
            if(err!=ESP_OK) { (void)stop_locked(slot);return err; }
        }
        slot->info.last_error = ESP_OK; slot->clock_started = true;
        if(!c->command_timeout_ms) slot->info.command_valid=true;
        slot->info.waiting=!slot->info.command_valid;
        slot->info.active_setpoint=0;
        slot->info.feedforward=0;
        slot->last_step_ms = solar_os_time_uptime_ms();
        /* Preserve manual mode, but discard derivative/integral history. */
        bool automatic = slot->info.state.automatic;
        bool tracking = slot->info.state.tracking;
        float initial = slot->info.state.output;
        solar_os_controller_math_reset(&c->algorithm, &slot->info.state, initial);
        slot->info.state.automatic = automatic;
        slot->info.state.tracking = tracking;
    }
    return err;
}

esp_err_t solar_os_controller_start(const char *name)
{
    lock(); controller_slot_t *slot = find(name);
    esp_err_t err = slot == NULL ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    if (slot != NULL && !slot->owner_id && !tuning_running(slot) && !slot->info.enabled && !slot->info.claimed) {
        err = start_locked(slot);
        if(err==ESP_OK) slot->tune_manual=false;
    }
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_controller_tune(const char *name, float band, uint32_t cycles, uint32_t timeout_ms)
{
    lock(); controller_slot_t *slot=find(name);
    esp_err_t err=slot?ESP_ERR_INVALID_STATE:ESP_ERR_NOT_FOUND;
    if(slot && (slot->owner_id || slot->info.config.algorithm.kind!=SOLAR_OS_CONTROLLER_PID)) err=ESP_ERR_NOT_SUPPORTED;
    else if(slot && !tuning_running(slot) && !slot->info.enabled && !slot->info.claimed) {
        const solar_os_controller_config_t *c=&slot->info.config;
        solar_os_controller_tune_config_t config={.setpoint=c->setpoint,
            .low=c->algorithm.output_min,.high=c->algorithm.output_max,
            .band=band==0?c->algorithm.hysteresis/2:band,.max_dt=c->algorithm.max_dt,
            .timeout=(float)timeout_ms/1000,.cycles=cycles,.reverse=c->algorithm.reverse};
        solar_os_controller_tuner_t candidate;
        bool manual=!c->target[0];
        err=ESP_ERR_INVALID_ARG;
        if(timeout_ms>=1000 && timeout_ms<=300000 &&
            c->safe_output>=config.low && c->safe_output<=config.high &&
            (!manual || !c->source[0]) && solar_os_controller_tuner_init(&candidate,&config)) {
            err=manual?ESP_OK:start_locked(slot);
            if(err==ESP_OK) {
                slot->tuner=candidate; slot->info.tuning=candidate.result; slot->tune_manual=manual;
                slot->tune_has_sample=false;
                slot->info.faulted=false; slot->info.last_error=ESP_OK;
                solar_os_controller_math_reset(&c->algorithm,&slot->info.state,c->safe_output);
            }
        }
    }
    xSemaphoreGive(mutex); return err;
}

static esp_err_t stop_locked(controller_slot_t *slot)
{
    if(tuning_running(slot)) {
        slot->info.tuning.state=SOLAR_OS_CONTROLLER_TUNE_ABORTED;
    }
    slot->info.enabled = false;
    slot->info.waiting = false;
    slot->info.command_valid=false;slot->info.command_expires_us=0;
    if (!slot->info.claimed) {
        slot->info.state.output=slot->info.config.safe_output;slot->info.feedforward=0;
        return restore_binding(slot);
    }
    esp_err_t err = slot->output_claimed?write_output(slot, slot->info.config.safe_output):ESP_OK;
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
        slot->info.state.output=slot->info.config.safe_output;slot->info.feedforward=0;
        err=restore_binding(slot);
        if(err!=ESP_OK) { slot->info.last_error=err;slot->info.faulted=true;return err; }
        /* Keep calibrated feedback pinned until motor release succeeds. The
         * encoder close cannot perform hardware I/O; retry retains its handle. */
        if(slot->source_persistent && slot->output_claimed) {
            err=release_output(slot);
            if(err!=ESP_OK) { slot->info.last_error=err;slot->info.faulted=true;return err; }
        }
        if (slot->source_open) {
            err = solar_os_stream_close_ex(&slot->source_handle);
            if (err == ESP_OK) slot->source_open = false;
        } else err = ESP_OK;
        if (err == ESP_OK && slot->output_claimed) err = release_output(slot);
    }
    slot->info.claimed=slot->output_claimed || slot->source_open || slot->parameter_claims;
    slot->info.last_error = err;
    if (err != ESP_OK) slot->info.faulted = true;
    return err;
}

esp_err_t solar_os_controller_stop(const char *name)
{
    lock(); controller_slot_t *slot = find(name);
    esp_err_t err = slot != NULL ? stop_locked(slot) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_controllers_stop_all(void)
{
    lock(); esp_err_t result = ESP_OK;
    for (size_t i = 0; i < SOLAR_OS_CONTROLLER_MAX; i++) if (slots[i].active) {
        esp_err_t err = stop_locked(&slots[i]); if (result == ESP_OK) result = err;
    }
    xSemaphoreGive(mutex); return result;
}

static void fault(controller_slot_t *slot, esp_err_t err)
{
    if(tuning_running(slot)) {
        slot->info.tuning.state=SOLAR_OS_CONTROLLER_TUNE_FAILED;
        slot->info.tuning.reason=SOLAR_OS_CONTROLLER_TUNE_BAD_SAMPLE;
        slot->info.state.output=slot->info.config.safe_output;
    }
    slot->info.enabled = false; slot->info.faulted = true; slot->info.faults++;
    slot->info.command_valid=slot->info.waiting=false;slot->info.command_expires_us=0;
    slot->info.last_error = err;
    /* Retain the claim until an explicit stop succeeds. */
    esp_err_t safe_err = slot->output_claimed?write_output(slot, slot->info.config.safe_output):ESP_OK;
    if(safe_err==ESP_OK) { slot->info.state.output=slot->info.config.safe_output;slot->info.feedforward=0; }
    if (safe_err != ESP_OK) slot->info.last_error = safe_err;
}

static controller_slot_t *find_owned(uint32_t id)
{
    for(unsigned i=0;id && i<SOLAR_OS_CONTROLLER_MAX;++i)
        if(slots[i].active && slots[i].owner_id==id) return &slots[i];
    return NULL;
}

esp_err_t solar_os_controller_open_owned(const char *name, const char *owner,
    const solar_os_native_controller_config_v1_t *c, uint32_t *id)
{
    if(id) *id=0;
    if(!id || !name || !*name || strnlen(name,SOLAR_OS_CONTROLLER_NAME_MAX)>=SOLAR_OS_CONTROLLER_NAME_MAX ||
        !owner || !*owner || strnlen(owner,16)>=16 || !c)
        return ESP_ERR_INVALID_ARG;
    solar_os_controller_math_config_t algorithm={.kind=SOLAR_OS_CONTROLLER_PID,
        .kp=c->kp,.ki=c->ki,.kd=c->kd,.derivative_tau=c->derivative_tau,
        .output_min=c->output_min,.output_max=c->output_max,.max_dt=c->max_dt,.hysteresis=1};
    if(!solar_os_controller_math_valid(&algorithm) || c->output_min>0 || c->output_max<0 ||
        !isfinite(c->setpoint_min) || !isfinite(c->setpoint_max) || c->setpoint_min>=c->setpoint_max ||
        c->setpoint_min>0 || c->setpoint_max<0 || c->period_ms<10 || c->period_ms>1000 ||
        c->timeout_ms<c->period_ms || c->timeout_ms>60000 || c->max_dt*1000<c->period_ms)
        return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t err=find(name)?ESP_ERR_INVALID_STATE:ESP_ERR_NO_MEM;
    if(err==ESP_ERR_NO_MEM && next_owner_id!=UINT32_MAX) for(unsigned i=0;i<SOLAR_OS_CONTROLLER_MAX;++i) {
        controller_slot_t *slot=&slots[i];
        if(slot->active) continue;
        *slot=(controller_slot_t){0};
        slot->source_handle=(solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
        slot->target_handle=(solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
        slot->info.config.algorithm=algorithm;
        strlcpy(slot->info.config.name,name,sizeof(slot->info.config.name));
        strlcpy(slot->info.owner,owner,sizeof(slot->info.owner));
        slot->info.config.setpoint_min=c->setpoint_min; slot->info.config.setpoint_max=c->setpoint_max;
        slot->info.config.period_ms=c->period_ms; slot->info.config.timeout_ms=c->timeout_ms;
        solar_os_controller_math_reset(&algorithm,&slot->info.state,0);
        slot->info.enabled=slot->info.waiting=slot->active=true;
        slot->owner_id=*id=++next_owner_id;
        err=ESP_OK; break;
    }
    xSemaphoreGive(mutex); return err;
}

static void fault_owned(controller_slot_t *slot, esp_err_t err)
{
    if(slot->managed) { if(!slot->info.faulted) fault(slot,err);return; }
    if(!slot->info.faulted) slot->info.faults++;
    slot->info.enabled=slot->info.waiting=false;
    slot->info.faulted=true; slot->info.last_error=err;
}

esp_err_t solar_os_controller_evaluate_owned(uint32_t id,
    const solar_os_native_controller_sample_v1_t *s, uint64_t now_us,
    solar_os_native_controller_output_v1_t *out)
{
    if(!s || !out) return ESP_ERR_INVALID_ARG;
    lock(); controller_slot_t *slot=find_owned(id);
    /* Another controller may have held this mutex across endpoint I/O. Age
     * the sample at completion of admission, not just before taking the lock. */
    uint64_t admitted_us=(uint64_t)esp_timer_get_time();
    if(admitted_us>now_us) now_us=admitted_us;
    esp_err_t err=slot?slot->info.enabled?ESP_OK:ESP_ERR_INVALID_STATE:ESP_ERR_NOT_FOUND;
    if(err==ESP_OK) {
        if(slot->managed) { xSemaphoreGive(mutex);return ESP_ERR_INVALID_STATE; }
        const solar_os_controller_config_t *c=&slot->info.config;
        if(!isfinite(s->setpoint) || s->setpoint<c->setpoint_min || s->setpoint>c->setpoint_max ||
            !isfinite(s->measurement) || !isfinite(s->dt) || s->dt<=0 || s->dt>c->algorithm.max_dt ||
            !isfinite(s->feedforward) || s->feedforward<c->algorithm.output_min ||
            s->feedforward>c->algorithm.output_max || s->run>1) err=ESP_ERR_INVALID_ARG;
        else if(s->sample_us>now_us || now_us-s->sample_us>(uint64_t)c->timeout_ms*1000 ||
            (slot->info.has_measurement && s->sample_us<=slot->sample_us)) err=ESP_ERR_TIMEOUT;
        if(err==ESP_OK) {
            solar_os_controller_math_config_t algorithm=c->algorithm;
            if(s->run) {
                algorithm.output_min-=s->feedforward; algorithm.output_max-=s->feedforward;
                if(!solar_os_controller_math_step(&algorithm,&slot->info.state,s->setpoint,s->measurement,s->dt))
                    err=ESP_ERR_INVALID_ARG;
            } else solar_os_controller_math_reset(&algorithm,&slot->info.state,0);
            if(err==ESP_OK) {
                slot->info.config.setpoint=s->setpoint;
                slot->info.measurement=s->measurement; slot->sample_us=s->sample_us;
                slot->info.sample_ms=s->sample_us/1000; slot->info.has_measurement=true;
                slot->info.waiting=!s->run; slot->info.feedforward=s->run?s->feedforward:0;
                slot->info.steps++; slot->info.last_error=ESP_OK;
                *out=(solar_os_native_controller_output_v1_t){
                    .output=fminf(c->algorithm.output_max,fmaxf(c->algorithm.output_min,
                        slot->info.state.output+slot->info.feedforward)),
                    .p=slot->info.state.p,.i=slot->info.state.i,.d=slot->info.state.d,
                    .saturated=slot->info.state.saturated};
            }
        }
        if(err!=ESP_OK) fault_owned(slot,err);
    }
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_controller_pause_owned(uint32_t id)
{
    lock(); controller_slot_t *slot=find_owned(id);
    esp_err_t err=slot?slot->info.enabled?ESP_OK:ESP_ERR_INVALID_STATE:ESP_ERR_NOT_FOUND;
    if(err==ESP_OK) {
        solar_os_controller_math_reset(&slot->info.config.algorithm,&slot->info.state,0);
        slot->info.config.setpoint=slot->info.feedforward=0; slot->info.waiting=true;
        if(slot->managed) {
            slot->info.command_valid=false;slot->info.command_expires_us=0;slot->info.active_setpoint=0;
            err=write_output(slot,0);
            if(err!=ESP_OK) fault(slot,err);
        }
    }
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_controller_fault_owned(uint32_t id, esp_err_t error)
{
    if(error==ESP_OK) return ESP_ERR_INVALID_ARG;
    lock(); controller_slot_t *slot=find_owned(id);
    if(slot) fault_owned(slot,error);
    xSemaphoreGive(mutex); return slot?ESP_OK:ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_controller_close_owned(uint32_t id)
{
    lock(); controller_slot_t *slot=find_owned(id);
    esp_err_t err=slot?ESP_OK:ESP_ERR_NOT_FOUND;
    if(slot && slot->managed) err=stop_locked(slot);
    if(slot && err==ESP_OK) *slot=(controller_slot_t){0};
    xSemaphoreGive(mutex); return err;
}

static void tick_locked(controller_slot_t *slot,uint64_t now_ms)
{
    const solar_os_controller_config_t *c = &slot->info.config;
    uint64_t elapsed = slot->clock_started ? now_ms - slot->last_step_ms : c->period_ms;
    if (slot->clock_started && now_ms < slot->last_step_ms) { fault(slot, ESP_ERR_TIMEOUT); return; }
    if (elapsed < c->period_ms) return;
    if (c->source[0] != '\0') {
        solar_os_stream_scalar_sample_t sample;
        esp_err_t err = slot->source_open?ESP_OK:solar_os_stream_open(c->source, "controllers", &slot->source_handle);
        if (err == ESP_OK) {
            slot->source_open = true;
            const solar_os_stream_read_options_t options = {.timeout_ms = c->period_ms};
            err = solar_os_stream_read_scalar_sample(&slot->source_handle, &options, &sample);
            if(!slot->source_persistent) {
                esp_err_t close_err = solar_os_stream_close_ex(&slot->source_handle);
                if (close_err == ESP_OK) slot->source_open = false;
                else err = close_err;
            }
        }
        if (err != ESP_OK) { fault(slot, err); return; }
        if (slot->info.has_measurement && sample.uptime_ms < slot->info.sample_ms) {
            fault(slot, ESP_ERR_INVALID_RESPONSE); return;
        }
        slot->info.measurement = sample.value; slot->info.sample_ms = sample.uptime_ms;
        slot->info.has_measurement = true;
        if(slot->source_persistent) {
            err=solar_os_encoder_feedback((uint32_t)slot->source_handle.private_data[0],&slot->info.feedback);
            if(err!=ESP_OK) { fault(slot,err);return; }
        }
    }
    /* Acquisition may have consumed time: validate age at completion too. */
    uint64_t completed_ms = solar_os_time_uptime_ms();
    if (completed_ms < now_ms) completed_ms = now_ms;
    if (!slot->info.has_measurement || slot->info.sample_ms > completed_ms ||
        completed_ms - slot->info.sample_ms > c->timeout_ms) { fault(slot, ESP_ERR_TIMEOUT); return; }
    float dt = (float)(slot->clock_started ? completed_ms - slot->last_step_ms : elapsed) / 1000;
    if(!isfinite(slot->info.measurement) || !isfinite(dt) || dt<=0) {
        fault(slot,ESP_ERR_INVALID_ARG);return;
    }
    if(dt>c->algorithm.max_dt) { fault(slot,ESP_ERR_TIMEOUT);return; }
    if(c->command_timeout_ms && slot->info.command_valid &&
        (uint64_t)esp_timer_get_time()>=slot->info.command_expires_us && !tuning_running(slot)) {
        expire_locked(slot);
    }
    slot->info.waiting=c->command_timeout_ms && !slot->info.command_valid && !tuning_running(slot);
    if(tuning_running(slot)) {
        /* A cached value is not another identification sample. Wait for a
         * new acquisition, with both freshness and step-gap limits intact. */
        if(slot->tune_has_sample && slot->info.sample_ms==slot->tune_sample_ms) {
            if(dt>c->algorithm.max_dt) fault(slot,ESP_ERR_TIMEOUT);
            else if(slot->tuner.elapsed+dt>=slot->tuner.config.timeout) {
                float output;
                esp_err_t err=tuning_step(slot,slot->info.measurement,dt,&output);
                fault(slot,err);
            }
            return;
        }
        slot->tune_sample_ms=slot->info.sample_ms; slot->tune_has_sample=true;
        float output;
        esp_err_t err=tuning_step(slot,slot->info.measurement,dt,&output);
        if(err!=ESP_OK) { fault(slot,err); return; }
        if(slot->info.tuning.state==SOLAR_OS_CONTROLLER_TUNE_COMPLETE) {
            err=stop_locked(slot);
            if(err!=ESP_OK) {
                slot->info.tuning.state=SOLAR_OS_CONTROLLER_TUNE_FAILED;
                slot->info.tuning.reason=SOLAR_OS_CONTROLLER_TUNE_CLEANUP;
                slot->info.tuning.kp=slot->info.tuning.ki=slot->info.tuning.kd=slot->info.tuning.derivative_tau=0;
            }
            return;
        }
    } else if(slot->info.waiting) {
        solar_os_controller_math_reset(&c->algorithm,&slot->info.state,c->safe_output);
        slot->info.active_setpoint=slot->info.feedforward=0;
    } else {
        float difference=c->setpoint-slot->info.active_setpoint;
        float change=c->acceleration_limit*dt;
        slot->info.active_setpoint=c->acceleration_limit>0?slot->info.active_setpoint+
            fminf(change,fmaxf(-change,difference)):c->setpoint;
        float ff=slot->info.state.automatic?(slot->info.active_setpoint>0?c->friction_bias_forward:
            slot->info.active_setpoint<0?-c->friction_bias_reverse:0):0;
        solar_os_controller_math_config_t algorithm=c->algorithm;
        algorithm.output_min-=ff;algorithm.output_max-=ff;
        if(!solar_os_controller_math_step(&algorithm,&slot->info.state,slot->info.active_setpoint,slot->info.measurement,dt)) {
            fault(slot,ESP_ERR_INVALID_ARG);return;
        }
        slot->info.feedforward=ff;
    }
    /* Account for time spent in acquisition and device status callbacks. */
    uint64_t before_write=solar_os_time_uptime_ms();
    if(before_write<completed_ms) before_write=completed_ms;
    if(before_write-slot->last_step_ms>c->algorithm.max_dt*1000 ||
        before_write-slot->info.sample_ms>c->timeout_ms) { fault(slot,ESP_ERR_TIMEOUT);return; }
    esp_err_t err = write_output(slot, slot->info.state.output+slot->info.feedforward);
    if(err==ESP_ERR_TIMEOUT && c->command_timeout_ms && slot->info.command_valid && !tuning_running(slot) &&
        (uint64_t)esp_timer_get_time()>=slot->info.command_expires_us) {
        uint64_t current=solar_os_time_uptime_ms();
        if(current>=slot->info.sample_ms && current-slot->info.sample_ms<=c->timeout_ms &&
            current>=slot->last_step_ms && current-slot->last_step_ms<=c->algorithm.max_dt*1000) {
            expire_locked(slot);err=write_output(slot,c->safe_output);
        }
    }
    if (err != ESP_OK) { fault(slot, err); return; }
    uint64_t after_write=solar_os_time_uptime_ms();
    if(after_write<before_write) after_write=before_write;
    if(after_write-slot->last_step_ms>c->algorithm.max_dt*1000 || after_write-slot->info.sample_ms>c->timeout_ms) {
        fault(slot,ESP_ERR_TIMEOUT);return;
    }
    if(!tuning_running(slot)) slot->info.steps++;
    if(!slot->info.waiting) slot->info.last_error = ESP_OK;
    slot->last_step_ms = completed_ms; slot->clock_started = true;
}

void solar_os_controllers_tick(uint64_t now_ms)
{
    lock();
    for(size_t i=0;i<SOLAR_OS_CONTROLLER_MAX;++i) {
        controller_slot_t *slot=&slots[i];
        if(slot->active && slot->info.enabled && !slot->owner_id) tick_locked(slot,now_ms);
    }
    xSemaphoreGive(mutex);
}

#include "solar_os_controllers_bound.inc"
