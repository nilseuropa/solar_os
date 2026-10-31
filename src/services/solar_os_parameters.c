#include "solar_os_parameters.h"
#include "solar_os_sync.h"

#include <math.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    bool active;
    bool retiring;
    unsigned references;
    uint32_t token;
    solar_os_parameter_info_t info;
    solar_os_parameter_get_fn get;
    solar_os_parameter_set_fn set;
    void *user;
} parameter_slot_t;

static EXT_RAM_BSS_ATTR parameter_slot_t
    parameter_slots[SOLAR_OS_PARAMETER_MAX];
static SemaphoreHandle_t parameter_mutex;
static StaticSemaphore_t parameter_mutex_storage;
static uint32_t parameter_next_token;

static esp_err_t parameter_ensure_mutex(void)
{
    (void)solar_os_mutex_get(&parameter_mutex, &parameter_mutex_storage);
    return parameter_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static uint32_t parameter_allocate_token(void)
{
    parameter_next_token++;
    if (parameter_next_token == 0U) {
        parameter_next_token++;
    }
    return parameter_next_token;
}

static bool parameter_component_valid(const char *text, size_t maximum)
{
    if (text == NULL || text[0] == '\0' || strnlen(text, maximum) >= maximum) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
               *p == '-' || *p == '_' || *p == '.')) {
            return false;
        }
    }
    return true;
}

static bool parameter_definition_valid(
    const solar_os_parameter_definition_t *definition)
{
    return definition != NULL &&
           parameter_component_valid(definition->name,
                                     SOLAR_OS_PARAMETER_NAME_MAX) &&
           definition->label != NULL &&
           strnlen(definition->label, SOLAR_OS_PARAMETER_LABEL_MAX) <
               SOLAR_OS_PARAMETER_LABEL_MAX &&
           definition->unit != NULL &&
           strnlen(definition->unit, SOLAR_OS_PARAMETER_UNIT_MAX) <
               SOLAR_OS_PARAMETER_UNIT_MAX &&
           isfinite(definition->minimum) && isfinite(definition->maximum) &&
           isfinite(definition->step) &&
           definition->maximum > definition->minimum &&
           definition->step >= 0.0f &&
           (definition->curve == SOLAR_OS_PARAMETER_CURVE_LINEAR ||
            (definition->curve == SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC &&
             definition->minimum > 0.0f)) &&
           definition->get != NULL && definition->set != NULL;
}

static int parameter_find_locked(const char *path)
{
    for (size_t i = 0; i < SOLAR_OS_PARAMETER_MAX; i++) {
        if (parameter_slots[i].active && !parameter_slots[i].retiring &&
            strcmp(parameter_slots[i].info.path, path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

esp_err_t solar_os_parameters_register(
    const char *owner,
    const solar_os_parameter_definition_t *definitions,
    size_t count,
    solar_os_parameter_registration_t *registration)
{
    if (!parameter_component_valid(owner, SOLAR_OS_PARAMETER_OWNER_MAX) ||
        definitions == NULL || count == 0U || count > SOLAR_OS_PARAMETER_MAX ||
        registration == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; i++) {
        if (!parameter_definition_valid(&definitions[i])) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = i + 1U; j < count; j++) {
            if (strcmp(definitions[i].name, definitions[j].name) == 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    esp_err_t err = parameter_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }

    *registration = (solar_os_parameter_registration_t)
        SOLAR_OS_PARAMETER_REGISTRATION_INIT;
    xSemaphoreTake(parameter_mutex, portMAX_DELAY);
    size_t free_count = 0U;
    for (size_t i = 0; i < SOLAR_OS_PARAMETER_MAX; i++) {
        free_count += parameter_slots[i].active ? 0U : 1U;
    }
    if (free_count < count) {
        xSemaphoreGive(parameter_mutex);
        return ESP_ERR_NO_MEM;
    }

    char paths[SOLAR_OS_PARAMETER_MAX][SOLAR_OS_PARAMETER_PATH_MAX];
    for (size_t i = 0; i < count; i++) {
        const int written = snprintf(paths[i], sizeof(paths[i]), "%s.%s",
                                     owner, definitions[i].name);
        if (written < 0 || (size_t)written >= sizeof(paths[i]) ||
            parameter_find_locked(paths[i]) >= 0) {
            xSemaphoreGive(parameter_mutex);
            return written < 0 || (size_t)written >= sizeof(paths[i]) ?
                ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_STATE;
        }
    }

    /* Retiring paths remain reserved until every borrowed callback returns. */
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < SOLAR_OS_PARAMETER_MAX; ++j) {
            if (parameter_slots[j].active &&
                strcmp(parameter_slots[j].info.path, paths[i]) == 0) {
                xSemaphoreGive(parameter_mutex);
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    const uint32_t token = parameter_allocate_token();
    size_t definition_index = 0U;
    for (size_t i = 0;
         i < SOLAR_OS_PARAMETER_MAX && definition_index < count;
         i++) {
        if (parameter_slots[i].active) {
            continue;
        }
        parameter_slot_t *slot = &parameter_slots[i];
        const solar_os_parameter_definition_t *definition =
            &definitions[definition_index];
        memset(slot, 0, sizeof(*slot));
        slot->active = true;
        slot->token = token;
        strlcpy(slot->info.path, paths[definition_index],
                sizeof(slot->info.path));
        strlcpy(slot->info.owner, owner, sizeof(slot->info.owner));
        strlcpy(slot->info.name, definition->name,
                sizeof(slot->info.name));
        strlcpy(slot->info.label, definition->label,
                sizeof(slot->info.label));
        strlcpy(slot->info.unit, definition->unit,
                sizeof(slot->info.unit));
        slot->info.minimum = definition->minimum;
        slot->info.maximum = definition->maximum;
        slot->info.step = definition->step;
        slot->info.curve = definition->curve;
        slot->get = definition->get;
        slot->set = definition->set;
        slot->user = definition->user;
        definition_index++;
    }
    registration->count = count;
    registration->token = token;
    xSemaphoreGive(parameter_mutex);
    return ESP_OK;
}

esp_err_t solar_os_parameters_unregister(
    solar_os_parameter_registration_t *registration)
{
    if (registration == NULL || registration->count == 0U ||
        registration->token == 0U ||
        parameter_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(parameter_mutex, portMAX_DELAY);
    size_t found = 0U;
    for (size_t i = 0; i < SOLAR_OS_PARAMETER_MAX; i++) {
        found += parameter_slots[i].active &&
                         parameter_slots[i].token == registration->token ?
                     1U : 0U;
    }
    if (found != registration->count) {
        xSemaphoreGive(parameter_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    bool busy = false;
    for (size_t i = 0; i < SOLAR_OS_PARAMETER_MAX; ++i) {
        if (parameter_slots[i].active &&
            parameter_slots[i].token == registration->token) {
            parameter_slots[i].retiring = true;
            busy |= parameter_slots[i].references != 0U;
        }
    }
    if (busy) {
        xSemaphoreGive(parameter_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < SOLAR_OS_PARAMETER_MAX; i++) {
        if (parameter_slots[i].active &&
            parameter_slots[i].token == registration->token) {
            memset(&parameter_slots[i], 0, sizeof(parameter_slots[0]));
        }
    }
    *registration = (solar_os_parameter_registration_t)
        SOLAR_OS_PARAMETER_REGISTRATION_INIT;
    xSemaphoreGive(parameter_mutex);
    return ESP_OK;
}

size_t solar_os_parameter_count(void)
{
    if (parameter_ensure_mutex() != ESP_OK) {
        return 0U;
    }
    xSemaphoreTake(parameter_mutex, portMAX_DELAY);
    size_t count = 0U;
    for (size_t i = 0; i < SOLAR_OS_PARAMETER_MAX; i++) {
        count += parameter_slots[i].active && !parameter_slots[i].retiring ? 1U : 0U;
    }
    xSemaphoreGive(parameter_mutex);
    return count;
}

bool solar_os_parameter_get_info(size_t index, solar_os_parameter_info_t *info)
{
    if (info == NULL || parameter_ensure_mutex() != ESP_OK) {
        return false;
    }
    xSemaphoreTake(parameter_mutex, portMAX_DELAY);
    size_t seen = 0U;
    for (size_t i = 0; i < SOLAR_OS_PARAMETER_MAX; i++) {
        if (!parameter_slots[i].active || parameter_slots[i].retiring) {
            continue;
        }
        if (seen++ == index) {
            *info = parameter_slots[i].info;
            xSemaphoreGive(parameter_mutex);
            return true;
        }
    }
    xSemaphoreGive(parameter_mutex);
    return false;
}

esp_err_t solar_os_parameter_find(const char *path,
                                  solar_os_parameter_info_t *info)
{
    if (path == NULL || path[0] == '\0' || info == NULL ||
        parameter_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(parameter_mutex, portMAX_DELAY);
    const int index = parameter_find_locked(path);
    if (index >= 0) {
        *info = parameter_slots[index].info;
    }
    xSemaphoreGive(parameter_mutex);
    return index >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

typedef struct {
    size_t index;
    parameter_slot_t slot;
} parameter_ref_t;

static esp_err_t parameter_acquire(const char *path, parameter_ref_t *ref)
{
    if (path == NULL || path[0] == '\0' ||
        parameter_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(parameter_mutex, portMAX_DELAY);
    const int index = parameter_find_locked(path);
    if (index < 0) {
        xSemaphoreGive(parameter_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (parameter_slots[index].references == UINT_MAX) {
        xSemaphoreGive(parameter_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    ++parameter_slots[index].references;
    ref->index = (size_t)index;
    ref->slot = parameter_slots[index];
    xSemaphoreGive(parameter_mutex);
    return ESP_OK;
}

static void parameter_release(const parameter_ref_t *ref)
{
    xSemaphoreTake(parameter_mutex, portMAX_DELAY);
    parameter_slot_t *slot = &parameter_slots[ref->index];
    if (slot->active && slot->token == ref->slot.token && slot->references)
        --slot->references;
    xSemaphoreGive(parameter_mutex);
}

static esp_err_t parameter_invoke(const parameter_ref_t *ref, bool write, float *value)
{
    const parameter_slot_t slot = ref->slot;
    if (!write) {
        return slot.get(slot.user, value);
    }
    if (!isfinite(*value) || *value < slot.info.minimum ||
        *value > slot.info.maximum) {
        return ESP_ERR_INVALID_ARG;
    }
    float adjusted = *value;
    if (slot.info.step > 0.0f) {
        adjusted = slot.info.minimum +
            roundf((adjusted - slot.info.minimum) / slot.info.step) *
                slot.info.step;
        if (adjusted > slot.info.maximum) {
            adjusted = slot.info.maximum;
        }
    }
    const esp_err_t err = slot.set(slot.user, adjusted);
    if (err == ESP_OK) {
        *value = adjusted;
    }
    return err;
}

esp_err_t solar_os_parameter_get(const char *path, float *value)
{
    if (value == NULL) return ESP_ERR_INVALID_ARG;
    parameter_ref_t ref;
    esp_err_t err = parameter_acquire(path, &ref);
    if (err == ESP_OK) {
        err = parameter_invoke(&ref, false, value);
        parameter_release(&ref);
    }
    return err;
}

esp_err_t solar_os_parameter_set(const char *path, float value)
{
    parameter_ref_t ref;
    esp_err_t err = parameter_acquire(path, &ref);
    if (err == ESP_OK) {
        err = parameter_invoke(&ref, true, &value);
        parameter_release(&ref);
    }
    return err;
}

static float parameter_from_normalized(const solar_os_parameter_info_t *info,
                                       uint16_t normalized)
{
    const float position = (float)normalized /
        (float)SOLAR_OS_PARAMETER_NORMALIZED_MAX;
    if (info->curve == SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC) {
        return info->minimum *
            powf(info->maximum / info->minimum, position);
    }
    return info->minimum + (info->maximum - info->minimum) * position;
}

static uint16_t parameter_to_normalized(const solar_os_parameter_info_t *info,
                                        float value)
{
    float position = 0.0f;
    if (info->curve == SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC) {
        position = logf(value / info->minimum) /
            logf(info->maximum / info->minimum);
    } else {
        position = (value - info->minimum) /
            (info->maximum - info->minimum);
    }
    if (position <= 0.0f) {
        return 0U;
    }
    if (position >= 1.0f) {
        return SOLAR_OS_PARAMETER_NORMALIZED_MAX;
    }
    return (uint16_t)lroundf(position *
                            (float)SOLAR_OS_PARAMETER_NORMALIZED_MAX);
}

esp_err_t solar_os_parameter_get_normalized(const char *path, uint16_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    parameter_ref_t ref;
    esp_err_t err = parameter_acquire(path, &ref);
    float raw = 0.0f;
    if (err == ESP_OK) {
        err = parameter_invoke(&ref, false, &raw);
        parameter_release(&ref);
    }
    if (err == ESP_OK) {
        if (!isfinite(raw) || raw < ref.slot.info.minimum || raw > ref.slot.info.maximum) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        *value = parameter_to_normalized(&ref.slot.info, raw);
    }
    return err;
}

esp_err_t solar_os_parameter_set_normalized(const char *path, uint16_t value)
{
    parameter_ref_t ref;
    esp_err_t err = parameter_acquire(path, &ref);
    if (err == ESP_OK) {
        float raw = parameter_from_normalized(&ref.slot.info, value);
        err = parameter_invoke(&ref, true, &raw);
        parameter_release(&ref);
    }
    return err;
}

const char *solar_os_parameter_curve_name(solar_os_parameter_curve_t curve)
{
    return curve == SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC ? "log" : "linear";
}
