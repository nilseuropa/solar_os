#include "solar_os_controllers.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_pwm.h"
#include "solar_os_time.h"

typedef struct {
    bool active;
    solar_os_controller_info_t info;
    uint64_t last_step_ms;
    bool clock_started;
    bool source_open;
    solar_os_stream_handle_t source_handle;
    solar_os_parameter_registration_t registration;
} controller_slot_t;

static EXT_RAM_BSS_ATTR controller_slot_t slots[SOLAR_OS_CONTROLLER_MAX];
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
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
    lock(); ((controller_slot_t *)user)->info.config.setpoint = value;
    xSemaphoreGive(mutex); return ESP_OK;
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
    } else {
        solar_os_pwm_output_t output;
        err = pwm_target(slot, &output) ? solar_os_pwm_close_output(&output, writer) : ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK) slot->info.claimed = false;
    return err;
}

esp_err_t solar_os_controller_create(const char *name, solar_os_controller_kind_t kind)
{
    if (!name_valid(name)) return ESP_ERR_INVALID_ARG;
    solar_os_controller_math_config_t algorithm = solar_os_controller_math_defaults(kind);
    if (!solar_os_controller_math_valid(&algorithm)) return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t err = find(name) != NULL ? ESP_ERR_INVALID_STATE : ESP_ERR_NO_MEM;
    if (err == ESP_ERR_NO_MEM) for (size_t i = 0; i < SOLAR_OS_CONTROLLER_MAX; i++) {
        controller_slot_t *slot = &slots[i];
        if (slot->active) continue;
        *slot = (controller_slot_t){0};
        slot->source_handle = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
        strlcpy(slot->info.config.name, name, sizeof(slot->info.config.name));
        slot->info.config.algorithm = algorithm;
        slot->info.config.setpoint_min = -1e20f;
        slot->info.config.setpoint_max = 1e20f;
        slot->info.config.period_ms = 20;
        slot->info.config.timeout_ms = 200;
        slot->info.config.frequency_hz = 50;
        solar_os_controller_math_reset(&algorithm, &slot->info.state, 0);
        char parameter[SOLAR_OS_PARAMETER_NAME_MAX];
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
    if (slot != NULL && !slot->info.enabled && !slot->info.claimed) {
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
        char path[SOLAR_OS_PARAMETER_PATH_MAX];
        snprintf(path, sizeof(path), "controller.%s.setpoint", name);
        return solar_os_parameter_set(path, value);
    }
    lock(); controller_slot_t *slot = find(name);
    esp_err_t err = slot == NULL ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    if (slot != NULL && !slot->info.enabled && !slot->info.claimed) {
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
        else if (value >= 1 && value <= 60000 && floorf(value) == value) {
            if (strcmp(key, "period_ms") == 0 && value >= 10) c.period_ms = (uint32_t)value;
            else if (strcmp(key, "timeout_ms") == 0) c.timeout_ms = (uint32_t)value;
            else if (strcmp(key, "frequency_hz") == 0 && value <= SOLAR_OS_PWM_FREQ_MAX_HZ) c.frequency_hz = (uint32_t)value;
            else err = ESP_ERR_INVALID_ARG;
        } else err = ESP_ERR_INVALID_ARG;
        if (!solar_os_controller_math_valid(&c.algorithm)) err = ESP_ERR_INVALID_ARG;
        if (c.setpoint_min >= c.setpoint_max || c.setpoint < c.setpoint_min ||
            c.setpoint > c.setpoint_max) err = ESP_ERR_INVALID_ARG;
        if (err == ESP_OK && (strcmp(key, "setpoint_min") == 0 || strcmp(key, "setpoint_max") == 0)) {
            char parameter[SOLAR_OS_PARAMETER_NAME_MAX];
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
        (strncmp(target, "parameter:", 10) != 0 && strncmp(target, "pwm:", 4) != 0) ||
        strncmp(target, "parameter:controller.", 21) == 0) return ESP_ERR_INVALID_ARG;
    lock(); controller_slot_t *slot = find(name);
    esp_err_t err = slot == NULL ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    if (slot != NULL && !slot->info.enabled && !slot->info.claimed) {
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
    if (slot != NULL && slot->info.config.source[0] == '\0') {
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
    if (slot != NULL && !slot->info.enabled && !slot->info.claimed) {
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
    if (slot != NULL && solar_os_controller_math_manual(&slot->info.config.algorithm, &slot->info.state, output)) err = ESP_OK;
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_controller_auto(const char *name)
{
    lock(); controller_slot_t *slot = find(name);
    if (slot != NULL) solar_os_controller_math_auto(&slot->info.state);
    xSemaphoreGive(mutex); return slot != NULL ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_controller_reset(const char *name)
{
    lock(); controller_slot_t *slot = find(name);
    esp_err_t err = slot == NULL ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    if (slot != NULL && !slot->info.enabled && !slot->info.claimed) {
        err = solar_os_controller_math_reset(&slot->info.config.algorithm, &slot->info.state,
            slot->info.config.safe_output) ? ESP_OK : ESP_ERR_INVALID_ARG;
        if (err == ESP_OK) {
            slot->info.faulted = false; slot->info.last_error = ESP_OK;
            slot->info.has_measurement = false;
        }
    }
    xSemaphoreGive(mutex); return err;
}

esp_err_t solar_os_controller_start(const char *name)
{
    lock(); controller_slot_t *slot = find(name);
    esp_err_t err = slot == NULL ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    if (slot != NULL && !slot->info.enabled && !slot->info.claimed) {
        const solar_os_controller_config_t *c = &slot->info.config;
        err = ESP_ERR_INVALID_ARG;
        if (c->safe_output >= c->algorithm.output_min && c->safe_output <= c->algorithm.output_max &&
            c->algorithm.max_dt * 1000 >= c->period_ms && c->timeout_ms >= c->period_ms) {
            char writer[SOLAR_OS_PARAMETER_WRITER_MAX]; writer_name(slot, writer, sizeof(writer));
            if (strncmp(c->target, "parameter:", 10) == 0) {
                solar_os_parameter_info_t info;
                err = solar_os_parameter_find(c->target + 10, &info);
                if (err == ESP_OK && (c->algorithm.output_min < info.minimum ||
                    c->algorithm.output_max > info.maximum)) err = ESP_ERR_INVALID_ARG;
                if (err == ESP_OK) err = solar_os_parameter_claim(c->target + 10, writer);
            } else {
                solar_os_pwm_output_t output;
                if (pwm_target(slot, &output) && c->algorithm.output_min >= 0 &&
                    (double)lroundf(c->algorithm.output_max) * c->frequency_hz < 1000000.0)
                    err = solar_os_pwm_open_output(&output, c->frequency_hz, writer);
            }
        }
        if (err == ESP_OK) {
            slot->info.enabled = slot->info.claimed = true; slot->info.faulted = false;
            slot->info.last_error = ESP_OK; slot->clock_started = true;
            slot->last_step_ms = solar_os_time_uptime_ms();
            /* Preserve manual mode, but discard derivative/integral history. */
            bool automatic = slot->info.state.automatic;
            bool tracking = slot->info.state.tracking;
            float initial = slot->info.state.output;
            solar_os_controller_math_reset(&c->algorithm, &slot->info.state, initial);
            slot->info.state.automatic = automatic;
            slot->info.state.tracking = tracking;
        }
    }
    xSemaphoreGive(mutex); return err;
}

static esp_err_t stop_locked(controller_slot_t *slot)
{
    slot->info.enabled = false;
    if (!slot->info.claimed) return ESP_OK;
    esp_err_t err = write_output(slot, slot->info.config.safe_output);
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
        if (slot->source_open) {
            err = solar_os_stream_close_ex(&slot->source_handle);
            if (err == ESP_OK) slot->source_open = false;
        } else err = ESP_OK;
        if (err == ESP_OK) err = release_output(slot);
    }
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
    slot->info.enabled = false; slot->info.faulted = true; slot->info.faults++;
    slot->info.last_error = err;
    /* Retain the claim until an explicit stop succeeds. */
    esp_err_t safe_err = write_output(slot, slot->info.config.safe_output);
    if (safe_err != ESP_OK) slot->info.last_error = safe_err;
}

void solar_os_controllers_tick(uint64_t now_ms)
{
    lock();
    for (size_t i = 0; i < SOLAR_OS_CONTROLLER_MAX; i++) {
        controller_slot_t *slot = &slots[i];
        if (!slot->active || !slot->info.enabled) continue;
        const solar_os_controller_config_t *c = &slot->info.config;
        uint64_t elapsed = slot->clock_started ? now_ms - slot->last_step_ms : c->period_ms;
        if (slot->clock_started && now_ms < slot->last_step_ms) { fault(slot, ESP_ERR_INVALID_ARG); continue; }
        if (elapsed < c->period_ms) continue;
        if (c->source[0] != '\0') {
            solar_os_stream_scalar_sample_t sample;
            esp_err_t err = solar_os_stream_open(c->source, "controllers", &slot->source_handle);
            if (err == ESP_OK) {
                slot->source_open = true;
                const solar_os_stream_read_options_t options = {.timeout_ms = c->period_ms};
                err = solar_os_stream_read_scalar_sample(&slot->source_handle, &options, &sample);
                esp_err_t close_err = solar_os_stream_close_ex(&slot->source_handle);
                if (close_err == ESP_OK) slot->source_open = false;
                else err = close_err;
            }
            if (err != ESP_OK) { fault(slot, err); continue; }
            if (slot->info.has_measurement && sample.uptime_ms < slot->info.sample_ms) {
                fault(slot, ESP_ERR_INVALID_RESPONSE); continue;
            }
            slot->info.measurement = sample.value; slot->info.sample_ms = sample.uptime_ms;
            slot->info.has_measurement = true;
        }
        /* Acquisition may have consumed time: validate age at completion too. */
        uint64_t completed_ms = solar_os_time_uptime_ms();
        if (completed_ms < now_ms) completed_ms = now_ms;
        if (!slot->info.has_measurement || slot->info.sample_ms > completed_ms ||
            completed_ms - slot->info.sample_ms > c->timeout_ms) { fault(slot, ESP_ERR_TIMEOUT); continue; }
        float dt = (float)(slot->clock_started ? completed_ms - slot->last_step_ms : elapsed) / 1000;
        if (!solar_os_controller_math_step(&c->algorithm, &slot->info.state,
            c->setpoint, slot->info.measurement, dt)) { fault(slot, ESP_ERR_INVALID_ARG); continue; }
        esp_err_t err = write_output(slot, slot->info.state.output);
        if (err != ESP_OK) { fault(slot, err); continue; }
        slot->info.steps++; slot->info.last_error = ESP_OK;
        slot->last_step_ms = completed_ms; slot->clock_started = true;
    }
    xSemaphoreGive(mutex);
}
