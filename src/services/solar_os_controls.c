#include "solar_os_controls.h"
#include "solar_os_sync.h"

#include <math.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    bool active;
    solar_os_control_info_t info;
    bool filter_initialized;
    float filtered_value;
    float published_value;
    uint32_t last_sample_ms;
} control_slot_t;

typedef struct {
    bool active;
    solar_os_control_binding_info_t info;
} binding_slot_t;

static EXT_RAM_BSS_ATTR control_slot_t control_slots[SOLAR_OS_CONTROL_MAX];
static EXT_RAM_BSS_ATTR binding_slot_t
    binding_slots[SOLAR_OS_CONTROL_BINDING_MAX];
static SemaphoreHandle_t controls_mutex;
static StaticSemaphore_t controls_mutex_storage;
static uint32_t next_binding_id;

static esp_err_t controls_ensure_mutex(void)
{
    (void)solar_os_mutex_get(&controls_mutex, &controls_mutex_storage);
    return controls_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool control_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0' ||
        strnlen(name, SOLAR_OS_CONTROL_NAME_MAX) >= SOLAR_OS_CONTROL_NAME_MAX) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
               *p == '-' || *p == '_')) {
            return false;
        }
    }
    return true;
}

static int control_find_locked(const char *name)
{
    for (size_t i = 0; i < SOLAR_OS_CONTROL_MAX; i++) {
        if (control_slots[i].active &&
            strcmp(control_slots[i].info.config.name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t binding_allocate_id(void)
{
    next_binding_id++;
    if (next_binding_id == 0U) {
        next_binding_id++;
    }
    return next_binding_id;
}

esp_err_t solar_os_control_create(const solar_os_control_config_t *config)
{
    if (config == NULL || !control_name_valid(config->name) ||
        strnlen(config->source, SOLAR_OS_STREAM_ID_MAX) >=
            SOLAR_OS_STREAM_ID_MAX ||
        !isfinite(config->input_minimum) || !isfinite(config->input_maximum) ||
        !isfinite(config->deadband) ||
        config->input_maximum <= config->input_minimum ||
        config->deadband < 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->source[0] != '\0') {
        solar_os_stream_info_t stream;
        const esp_err_t stream_err = solar_os_stream_get_info(config->source,
                                                               &stream);
        if (stream_err != ESP_OK) {
            return stream_err;
        }
        if (stream.type != SOLAR_OS_STREAM_TYPE_SCALAR) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    esp_err_t err = controls_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(controls_mutex, portMAX_DELAY);
    if (control_find_locked(config->name) >= 0) {
        xSemaphoreGive(controls_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < SOLAR_OS_CONTROL_MAX; i++) {
        if (control_slots[i].active) {
            continue;
        }
        memset(&control_slots[i], 0, sizeof(control_slots[i]));
        control_slots[i].active = true;
        control_slots[i].info.config = *config;
        xSemaphoreGive(controls_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(controls_mutex);
    return ESP_ERR_NO_MEM;
}

esp_err_t solar_os_control_delete(const char *name)
{
    if (name == NULL || controls_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(controls_mutex, portMAX_DELAY);
    const int index = control_find_locked(name);
    if (index < 0) {
        xSemaphoreGive(controls_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    memset(&control_slots[index], 0, sizeof(control_slots[index]));
    for (size_t i = 0; i < SOLAR_OS_CONTROL_BINDING_MAX; i++) {
        if (binding_slots[i].active &&
            strcmp(binding_slots[i].info.control, name) == 0) {
            memset(&binding_slots[i], 0, sizeof(binding_slots[i]));
        }
    }
    xSemaphoreGive(controls_mutex);
    return ESP_OK;
}

void solar_os_control_clear(void)
{
    if (controls_ensure_mutex() != ESP_OK) {
        return;
    }
    xSemaphoreTake(controls_mutex, portMAX_DELAY);
    memset(control_slots, 0, sizeof(control_slots));
    memset(binding_slots, 0, sizeof(binding_slots));
    next_binding_id = 0U;
    xSemaphoreGive(controls_mutex);
}

size_t solar_os_control_count(void)
{
    if (controls_ensure_mutex() != ESP_OK) {
        return 0U;
    }
    xSemaphoreTake(controls_mutex, portMAX_DELAY);
    size_t count = 0U;
    for (size_t i = 0; i < SOLAR_OS_CONTROL_MAX; i++) {
        count += control_slots[i].active ? 1U : 0U;
    }
    xSemaphoreGive(controls_mutex);
    return count;
}

bool solar_os_control_get_info(size_t index, solar_os_control_info_t *info)
{
    if (info == NULL || controls_ensure_mutex() != ESP_OK) {
        return false;
    }
    xSemaphoreTake(controls_mutex, portMAX_DELAY);
    size_t seen = 0U;
    for (size_t i = 0; i < SOLAR_OS_CONTROL_MAX; i++) {
        if (!control_slots[i].active) {
            continue;
        }
        if (seen++ == index) {
            *info = control_slots[i].info;
            xSemaphoreGive(controls_mutex);
            return true;
        }
    }
    xSemaphoreGive(controls_mutex);
    return false;
}

esp_err_t solar_os_control_find(const char *name, solar_os_control_info_t *info)
{
    if (name == NULL || info == NULL || controls_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(controls_mutex, portMAX_DELAY);
    const int index = control_find_locked(name);
    if (index >= 0) {
        *info = control_slots[index].info;
    }
    xSemaphoreGive(controls_mutex);
    return index >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_control_get(const char *name, uint16_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_control_info_t info;
    const esp_err_t err = solar_os_control_find(name, &info);
    if (err == ESP_OK && !info.has_value) {
        return ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        *value = info.normalized;
    }
    return err;
}

esp_err_t solar_os_control_set(const char *name, uint16_t value)
{
    if (name == NULL || controls_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(controls_mutex, portMAX_DELAY);
    const int index = control_find_locked(name);
    if (index < 0) {
        xSemaphoreGive(controls_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    solar_os_control_info_t *info = &control_slots[index].info;
    if (!info->has_value || info->normalized != value) {
        info->has_value = true;
        info->normalized = value;
        info->generation++;
        info->updates++;
    }
    info->last_error = ESP_OK;
    xSemaphoreGive(controls_mutex);
    return ESP_OK;
}

esp_err_t solar_os_control_publish_sample(const char *name,
                                          float source_value,
                                          uint32_t now_ms)
{
    if (name == NULL || !isfinite(source_value) ||
        controls_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(controls_mutex, portMAX_DELAY);
    const int index = control_find_locked(name);
    if (index < 0) {
        xSemaphoreGive(controls_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    control_slot_t *slot = &control_slots[index];
    const solar_os_control_config_t *config = &slot->info.config;
    slot->info.samples++;
    if (!slot->filter_initialized) {
        slot->filtered_value = source_value;
        slot->filter_initialized = true;
    } else if (config->smoothing_ms > 0U) {
        uint32_t elapsed = now_ms - slot->last_sample_ms;
        if (elapsed == 0U) {
            elapsed = 1U;
        }
        const float alpha = elapsed >= config->smoothing_ms ? 1.0f :
            (float)elapsed / (float)config->smoothing_ms;
        slot->filtered_value += (source_value - slot->filtered_value) * alpha;
    } else {
        slot->filtered_value = source_value;
    }
    slot->last_sample_ms = now_ms;

    if (slot->info.has_value && config->deadband > 0.0f &&
        fabsf(slot->filtered_value - slot->published_value) < config->deadband) {
        slot->info.last_error = ESP_OK;
        xSemaphoreGive(controls_mutex);
        return ESP_OK;
    }
    float position = (slot->filtered_value - config->input_minimum) /
        (config->input_maximum - config->input_minimum);
    if (position < 0.0f) {
        position = 0.0f;
    } else if (position > 1.0f) {
        position = 1.0f;
    }
    if (config->inverted) {
        position = 1.0f - position;
    }
    const uint16_t normalized = (uint16_t)lroundf(
        position * (float)SOLAR_OS_CONTROL_NORMALIZED_MAX);
    slot->published_value = slot->filtered_value;
    slot->info.source_value = slot->filtered_value;
    slot->info.last_error = ESP_OK;
    if (!slot->info.has_value || slot->info.normalized != normalized) {
        slot->info.has_value = true;
        slot->info.normalized = normalized;
        slot->info.generation++;
        slot->info.updates++;
    }
    xSemaphoreGive(controls_mutex);
    return ESP_OK;
}

void solar_os_control_note_read_error(const char *name, esp_err_t error)
{
    if (name == NULL || controls_ensure_mutex() != ESP_OK) {
        return;
    }
    xSemaphoreTake(controls_mutex, portMAX_DELAY);
    const int index = control_find_locked(name);
    if (index >= 0) {
        control_slots[index].info.read_errors++;
        control_slots[index].info.last_error = error;
    }
    xSemaphoreGive(controls_mutex);
}

static esp_err_t control_bind(const char *control,
                              solar_os_control_target_t target,
                              const char *parameter,
                              uint8_t channel,
                              uint8_t controller,
                              bool pickup,
                              uint32_t *binding_id)
{
    if (!control_name_valid(control) ||
        (target == SOLAR_OS_CONTROL_TARGET_PARAMETER &&
         (parameter == NULL || parameter[0] == '\0' ||
          strnlen(parameter, SOLAR_OS_PARAMETER_PATH_MAX) >=
              SOLAR_OS_PARAMETER_PATH_MAX)) ||
        (target == SOLAR_OS_CONTROL_TARGET_MIDI_CC &&
         (channel < 1U || channel > 16U || controller > 127U)) ||
        controls_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(controls_mutex, portMAX_DELAY);
    if (control_find_locked(control) < 0) {
        xSemaphoreGive(controls_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t i = 0; i < SOLAR_OS_CONTROL_BINDING_MAX; i++) {
        if (binding_slots[i].active) {
            continue;
        }
        memset(&binding_slots[i], 0, sizeof(binding_slots[i]));
        binding_slots[i].active = true;
        binding_slots[i].info.id = binding_allocate_id();
        strlcpy(binding_slots[i].info.control, control,
                sizeof(binding_slots[i].info.control));
        binding_slots[i].info.target = target;
        if (parameter != NULL) {
            strlcpy(binding_slots[i].info.parameter, parameter,
                    sizeof(binding_slots[i].info.parameter));
        }
        binding_slots[i].info.midi_channel = channel;
        binding_slots[i].info.midi_controller = controller;
        binding_slots[i].info.pickup = pickup;
        if (binding_id != NULL) {
            *binding_id = binding_slots[i].info.id;
        }
        xSemaphoreGive(controls_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(controls_mutex);
    return ESP_ERR_NO_MEM;
}

esp_err_t solar_os_control_bind_parameter(const char *control,
                                          const char *parameter,
                                          bool pickup,
                                          uint32_t *binding_id)
{
    return control_bind(control, SOLAR_OS_CONTROL_TARGET_PARAMETER, parameter,
                        0U, 0U, pickup, binding_id);
}

esp_err_t solar_os_control_bind_midi_cc(const char *control,
                                       uint8_t channel,
                                       uint8_t controller,
                                       uint32_t *binding_id)
{
    return control_bind(control, SOLAR_OS_CONTROL_TARGET_MIDI_CC, NULL,
                        channel, controller, false, binding_id);
}

esp_err_t solar_os_control_unbind(const char *control, size_t *removed)
{
    if (!control_name_valid(control) || controls_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (removed != NULL) {
        *removed = 0U;
    }
    xSemaphoreTake(controls_mutex, portMAX_DELAY);
    if (control_find_locked(control) < 0) {
        xSemaphoreGive(controls_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    size_t count = 0U;
    for (size_t i = 0; i < SOLAR_OS_CONTROL_BINDING_MAX; i++) {
        if (binding_slots[i].active &&
            strcmp(binding_slots[i].info.control, control) == 0) {
            memset(&binding_slots[i], 0, sizeof(binding_slots[i]));
            count++;
        }
    }
    xSemaphoreGive(controls_mutex);
    if (removed != NULL) {
        *removed = count;
    }
    return count > 0U ? ESP_OK : ESP_ERR_NOT_FOUND;
}

size_t solar_os_control_binding_count(void)
{
    if (controls_ensure_mutex() != ESP_OK) {
        return 0U;
    }
    xSemaphoreTake(controls_mutex, portMAX_DELAY);
    size_t count = 0U;
    for (size_t i = 0; i < SOLAR_OS_CONTROL_BINDING_MAX; i++) {
        count += binding_slots[i].active ? 1U : 0U;
    }
    xSemaphoreGive(controls_mutex);
    return count;
}

bool solar_os_control_binding_get(size_t index,
                                  solar_os_control_binding_info_t *info)
{
    if (info == NULL || controls_ensure_mutex() != ESP_OK) {
        return false;
    }
    xSemaphoreTake(controls_mutex, portMAX_DELAY);
    size_t seen = 0U;
    for (size_t i = 0; i < SOLAR_OS_CONTROL_BINDING_MAX; i++) {
        if (!binding_slots[i].active) {
            continue;
        }
        if (seen++ == index) {
            *info = binding_slots[i].info;
            xSemaphoreGive(controls_mutex);
            return true;
        }
    }
    xSemaphoreGive(controls_mutex);
    return false;
}

void solar_os_control_binding_note(uint32_t binding_id,
                                   uint32_t generation,
                                   bool pickup_seen,
                                   bool pickup_latched,
                                   uint16_t pickup_previous,
                                   uint16_t target_value,
                                   esp_err_t error,
                                   bool applied)
{
    if (binding_id == 0U || controls_ensure_mutex() != ESP_OK) {
        return;
    }
    xSemaphoreTake(controls_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_CONTROL_BINDING_MAX; i++) {
        if (!binding_slots[i].active ||
            binding_slots[i].info.id != binding_id) {
            continue;
        }
        solar_os_control_binding_info_t *info = &binding_slots[i].info;
        info->pickup_seen = pickup_seen;
        info->pickup_latched = pickup_latched;
        info->pickup_previous = pickup_previous;
        info->last_target_value = target_value;
        info->last_error = error;
        if (applied) {
            info->last_generation = generation;
            info->applied++;
        } else if (error != ESP_OK && error != ESP_ERR_NOT_FOUND &&
                   error != ESP_ERR_INVALID_STATE) {
            info->errors++;
        }
        break;
    }
    xSemaphoreGive(controls_mutex);
}

const char *solar_os_control_target_name(solar_os_control_target_t target)
{
    return target == SOLAR_OS_CONTROL_TARGET_MIDI_CC ? "midi" : "parameter";
}
