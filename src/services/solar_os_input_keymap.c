#include "solar_os_input_keymap.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_input_composition.h"
#include "solar_os_input_keymap_json.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

#define PROVIDER_MAX 8U
#define UNREGISTER_WAIT_MS 500U
#define KEYMAP_FILE_MAX 16384U

typedef struct {
    solar_os_input_keymap_t map, defaults;
    solar_os_input_keymap_state_t state;
} keymap_data_t;

typedef struct {
    solar_os_input_keymap_handle_t handle;
    char name[SOLAR_OS_INPUT_SOURCE_NAME_MAX];
    solar_os_input_keymap_info_t info;
    solar_os_input_keymap_ops_t ops;
    keymap_data_t *data;
    unsigned references;
    bool enabled, closing, unregistering;
} keymap_provider_t;

static keymap_provider_t providers[PROVIDER_MAX];
static portMUX_TYPE registry_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t next_handle;

static keymap_provider_t *find_handle(solar_os_input_keymap_handle_t handle)
{
    if (handle != 0U)
        for (unsigned i = 0; i < PROVIDER_MAX; i++)
            if (providers[i].handle == handle) return &providers[i];
    return NULL;
}

static keymap_provider_t *acquire_handle(solar_os_input_keymap_handle_t handle)
{
    portENTER_CRITICAL(&registry_lock);
    keymap_provider_t *p = find_handle(handle);
    if (p != NULL && p->enabled && !p->closing) p->references++;
    else p = NULL;
    portEXIT_CRITICAL(&registry_lock);
    return p;
}

static void release_reference(keymap_provider_t *p)
{
    portENTER_CRITICAL(&registry_lock);
    p->references--;
    portEXIT_CRITICAL(&registry_lock);
}

static esp_err_t acquire_name(const char *name, keymap_provider_t **out)
{
    if (name == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    solar_os_input_source_info_t source;
    if (!solar_os_input_source_find(name, &source)) return ESP_ERR_NOT_FOUND;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    portENTER_CRITICAL(&registry_lock);
    for (unsigned i = 0; i < PROVIDER_MAX; i++) {
        keymap_provider_t *p = &providers[i];
        if (p->handle != 0U && p->info.source == source.source && strcmp(p->name, name) == 0) {
            err = p->closing || !p->enabled ? ESP_ERR_INVALID_STATE : ESP_OK;
            if (err == ESP_OK) { p->references++; *out = p; }
            break;
        }
    }
    portEXIT_CRITICAL(&registry_lock);
    return err;
}

static esp_err_t begin_operation(keymap_provider_t *p)
{
    esp_err_t err = p->ops.lock(p->ops.context);
    if (err != ESP_OK) return err;
    portENTER_CRITICAL(&registry_lock);
    const bool closing = p->closing;
    portEXIT_CRITICAL(&registry_lock);
    if (closing) { p->ops.unlock(p->ops.context); return ESP_ERR_INVALID_STATE; }
    return ESP_OK;
}

static void release_keys(keymap_provider_t *p)
{
    solar_os_input_source_release_all(p->info.source);
    solar_os_input_composition_set_modifiers(p->info.source, 0U);
    solar_os_input_keymap_release(&p->data->state);
}

esp_err_t solar_os_input_keymap_register(solar_os_input_source_t source,
    const solar_os_input_keymap_t *defaults, const solar_os_input_keymap_ops_t *ops,
    solar_os_input_keymap_handle_t *handle)
{
    solar_os_input_source_info_t input;
    if (handle == NULL || ops == NULL || ops->lock == NULL || ops->unlock == NULL ||
        solar_os_input_keymap_validate(defaults) != ESP_OK ||
        !solar_os_input_source_get_info(source, &input) ||
        (input.capabilities & SOLAR_OS_INPUT_CAP_KEY_EVENTS) == 0U) return ESP_ERR_INVALID_ARG;
    keymap_data_t *data = solar_os_memory_alloc(sizeof(*data),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "input-keymap");
    if (data == NULL) return ESP_ERR_NO_MEM;
    memset(data, 0, sizeof(*data));
    data->map = data->defaults = *defaults;
    esp_err_t err = ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&registry_lock);
    keymap_provider_t *free_provider = NULL;
    for (unsigned i = 0; i < PROVIDER_MAX; i++) {
        if (providers[i].handle == 0U && free_provider == NULL) free_provider = &providers[i];
        if (providers[i].handle != 0U && providers[i].info.source == source) {
            err = ESP_ERR_INVALID_STATE; free_provider = NULL; break;
        }
    }
    if (free_provider != NULL) {
        do { next_handle++; } while (next_handle == 0U || find_handle(next_handle) != NULL);
        keymap_provider_t *p = free_provider;
        memset(p, 0, sizeof(*p));
        p->handle = next_handle; p->data = data; p->ops = *ops;
        strlcpy(p->name, input.name, sizeof(p->name));
        p->info.source = source;
        p->info.capabilities = SOLAR_OS_INPUT_KEYMAP_PHYSICAL | SOLAR_OS_INPUT_KEYMAP_MODIFIERS |
            SOLAR_OS_INPUT_KEYMAP_LAYERS | SOLAR_OS_INPUT_KEYMAP_TAP_HOLD;
        if (defaults->rows != 0U) p->info.capabilities |= SOLAR_OS_INPUT_KEYMAP_MATRIX;
        p->info.rows = defaults->rows; p->info.cols = defaults->cols;
        p->info.first = defaults->first; p->info.stride = defaults->stride;
        for (unsigned i = 1; i <= defaults->slot_count; i++)
            if (defaults->physical[i] != 0U) p->info.key_count++;
        *handle = p->handle;
        err = ESP_OK;
    }
    portEXIT_CRITICAL(&registry_lock);
    if (err != ESP_OK) free(data);
    return err;
}

esp_err_t solar_os_input_keymap_activate(solar_os_input_keymap_handle_t handle)
{
    portENTER_CRITICAL(&registry_lock);
    keymap_provider_t *p = find_handle(handle);
    esp_err_t err = p == NULL ? ESP_ERR_NOT_FOUND : p->closing ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK) p->enabled = true;
    portEXIT_CRITICAL(&registry_lock);
    return err;
}

esp_err_t solar_os_input_keymap_unregister(solar_os_input_keymap_handle_t handle)
{
    portENTER_CRITICAL(&registry_lock);
    keymap_provider_t *p = find_handle(handle);
    esp_err_t err = p == NULL ? ESP_ERR_NOT_FOUND :
        p->unregistering ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK) { p->closing = true; p->unregistering = true; }
    portEXIT_CRITICAL(&registry_lock);
    if (err != ESP_OK) return err;
    const TickType_t start = xTaskGetTickCount();
    for (;;) {
        portENTER_CRITICAL(&registry_lock);
        const unsigned references = p->references;
        portEXIT_CRITICAL(&registry_lock);
        if (references == 0U) break;
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(UNREGISTER_WAIT_MS)) {
            portENTER_CRITICAL(&registry_lock);
            p->unregistering = false;
            portEXIT_CRITICAL(&registry_lock);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1U) > 0U ? pdMS_TO_TICKS(1U) : 1U);
    }
    if (p->enabled) release_keys(p);
    keymap_data_t *data = p->data;
    portENTER_CRITICAL(&registry_lock);
    memset(p, 0, sizeof(*p));
    portEXIT_CRITICAL(&registry_lock);
    free(data);
    return ESP_OK;
}

esp_err_t solar_os_input_keymap_info(const char *name, solar_os_input_keymap_info_t *info)
{
    if (name == NULL || info == NULL) return ESP_ERR_INVALID_ARG;
    solar_os_input_source_info_t source;
    if (!solar_os_input_source_find(name, &source)) return ESP_ERR_NOT_FOUND;
    memset(info, 0, sizeof(*info)); info->source = source.source;
    portENTER_CRITICAL(&registry_lock);
    for (unsigned i = 0; i < PROVIDER_MAX; i++)
        if (providers[i].handle != 0U && providers[i].enabled && !providers[i].closing &&
            providers[i].info.source == source.source && strcmp(providers[i].name, name) == 0) {
            *info = providers[i].info; break;
        }
    portEXIT_CRITICAL(&registry_lock);
    return ESP_OK;
}

esp_err_t solar_os_input_keymap_process(solar_os_input_keymap_handle_t handle,
    uint16_t physical, bool pressed, bool *published)
{
    if (published != NULL) *published = false;
    keymap_provider_t *p = acquire_handle(handle);
    if (p == NULL) return ESP_ERR_INVALID_STATE;
    solar_os_input_keymap_transition_t t;
    esp_err_t err = ESP_OK;
    if (solar_os_input_keymap_decode(&p->data->state, &p->data->map, physical, pressed, &t)) {
        solar_os_input_composition_set_modifiers(p->info.source, t.modifiers);
        uint8_t key = t.key != 0U ? t.key : solar_os_input_translate_hid_usage(
            t.usage, t.modifiers, p->data->state.caps_lock);
        key = solar_os_input_composition_apply(key);
        err = solar_os_input_write_key(p->info.source, physical, t.usage, key, t.modifiers,
            pressed ? SOLAR_OS_INPUT_KEY_PRESS : SOLAR_OS_INPUT_KEY_RELEASE);
        if (err == ESP_OK && (t.tap_usage != 0U || t.tap_key != 0U)) {
            uint8_t tap = t.tap_key != 0U ? t.tap_key : solar_os_input_translate_hid_usage(
                t.tap_usage, t.modifiers, p->data->state.caps_lock);
            tap = solar_os_input_composition_apply(tap);
            if (tap != 0U) err = solar_os_input_write_char(p->info.source, tap);
        }
        if (published != NULL) *published = err == ESP_OK;
    }
    release_reference(p);
    return err;
}

void solar_os_input_keymap_release_all(solar_os_input_keymap_handle_t handle)
{
    keymap_provider_t *p = acquire_handle(handle);
    if (p != NULL) { release_keys(p); release_reference(p); }
}

static esp_err_t apply_map(keymap_provider_t *p, const solar_os_input_keymap_t *map)
{
    const solar_os_input_keymap_t *base = &p->data->defaults;
    if (solar_os_input_keymap_validate(map) != ESP_OK || map->slot_count != base->slot_count ||
        map->rows != base->rows || map->cols != base->cols || map->first != base->first ||
        map->stride != base->stride || memcmp(map->physical, base->physical, sizeof(map->physical)) != 0)
        return ESP_ERR_INVALID_ARG;
    release_keys(p);
    esp_err_t err = p->ops.prepare != NULL ? p->ops.prepare(p->ops.context) : ESP_OK;
    if (err == ESP_OK) {
        memset(&p->data->state, 0, sizeof(p->data->state));
        p->data->map = *map;
    }
    return err;
}

esp_err_t solar_os_input_keymap_get(const char *name, bool defaults, solar_os_input_keymap_t *map)
{
    if (map == NULL) return ESP_ERR_INVALID_ARG;
    keymap_provider_t *p = NULL;
    esp_err_t err = acquire_name(name, &p);
    if (err != ESP_OK) return err;
    err = begin_operation(p);
    if (err == ESP_OK) { *map = defaults ? p->data->defaults : p->data->map; p->ops.unlock(p->ops.context); }
    release_reference(p);
    return err;
}

esp_err_t solar_os_input_keymap_set(const char *name, const solar_os_input_keymap_t *map)
{
    if (solar_os_input_keymap_validate(map) != ESP_OK) return ESP_ERR_INVALID_ARG;
    keymap_provider_t *p = NULL;
    esp_err_t err = acquire_name(name, &p);
    if (err != ESP_OK) return err;
    err = begin_operation(p);
    if (err == ESP_OK) { err = apply_map(p, map); p->ops.unlock(p->ops.context); }
    release_reference(p);
    return err;
}

esp_err_t solar_os_input_keymap_reset(const char *name)
{
    keymap_provider_t *p = NULL;
    esp_err_t err = acquire_name(name, &p);
    if (err != ESP_OK) return err;
    err = begin_operation(p);
    if (err == ESP_OK) { err = apply_map(p, &p->data->defaults); p->ops.unlock(p->ops.context); }
    release_reference(p);
    return err;
}

esp_err_t solar_os_input_keymap_load(const char *name, const char *path)
{
    if (path == NULL || path[0] != '/') return ESP_ERR_INVALID_ARG;
    keymap_provider_t *p = NULL;
    esp_err_t err = acquire_name(name, &p);
    if (err != ESP_OK) return err;
    char *json = solar_os_memory_alloc(KEYMAP_FILE_MAX + 2U,
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "keyboard-map");
    solar_os_input_keymap_t *map = solar_os_memory_alloc(sizeof(*map),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "keyboard-map");
    if (json == NULL || map == NULL) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) *map = p->data->defaults; /* Immutable for this registered generation. */
    size_t length = 0U;
    if (err == ESP_OK) err = solar_os_storage_read_file(path, json, KEYMAP_FILE_MAX + 1U, &length);
    if (err == ESP_OK && length > KEYMAP_FILE_MAX) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK && memchr(json, '\0', length) != NULL) err = ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) { json[length] = '\0'; err = solar_os_input_keymap_parse(json, map, map); }
    if (err == ESP_OK) {
        err = begin_operation(p);
        if (err == ESP_OK) { err = apply_map(p, map); p->ops.unlock(p->ops.context); }
    }
    free(map); free(json);
    release_reference(p);
    return err;
}
