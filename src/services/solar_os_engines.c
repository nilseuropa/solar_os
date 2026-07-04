#include "solar_os_engines.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_board_caps.h"

#define SOLAR_OS_ENGINE_MAX 12

typedef struct {
    bool registered;
    char name[SOLAR_OS_ENGINE_NAME_MAX];
    char class_name[SOLAR_OS_ENGINE_CLASS_MAX];
    char owner[SOLAR_OS_ENGINE_OWNER_MAX];
    char label[SOLAR_OS_ENGINE_LABEL_MAX];
    uint32_t active_count;
    uint32_t generation;
    int64_t active_started_us;
    uint64_t op_count;
    uint64_t unit_count;
    uint64_t work_us;
    uint64_t busy_us;
    uint64_t max_us;
    uint64_t last_us;
    uint64_t last_units;
} engine_slot_t;

static engine_slot_t engines[SOLAR_OS_ENGINE_MAX];
static SemaphoreHandle_t engines_mutex;
static bool engines_initialized;
static int64_t engines_epoch_us;

static int64_t engine_now_us(void)
{
    return esp_timer_get_time();
}

static uint64_t elapsed_us(int64_t start, int64_t end)
{
    return end > start ? (uint64_t)(end - start) : 0U;
}

static int engine_find_locked(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return -1;
    }

    for (size_t i = 0; i < SOLAR_OS_ENGINE_MAX; i++) {
        if (engines[i].registered &&
            strncmp(engines[i].name, name, sizeof(engines[i].name)) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static esp_err_t engine_register_locked(const char *name, const char *class_name)
{
    if (name == NULL || name[0] == '\0' || strlen(name) >= SOLAR_OS_ENGINE_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (class_name != NULL && strlen(class_name) >= SOLAR_OS_ENGINE_CLASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    int existing = engine_find_locked(name);
    if (existing >= 0) {
        strlcpy(engines[existing].class_name,
                class_name != NULL ? class_name : "",
                sizeof(engines[existing].class_name));
        return ESP_OK;
    }

    for (size_t i = 0; i < SOLAR_OS_ENGINE_MAX; i++) {
        if (engines[i].registered) {
            continue;
        }
        engines[i].registered = true;
        strlcpy(engines[i].name, name, sizeof(engines[i].name));
        strlcpy(engines[i].class_name,
                class_name != NULL ? class_name : "",
                sizeof(engines[i].class_name));
        return ESP_OK;
    }

    return ESP_ERR_NO_MEM;
}

static esp_err_t engines_ensure_init(void)
{
    if (engines_mutex == NULL) {
        engines_mutex = xSemaphoreCreateMutex();
        if (engines_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(engines_mutex, portMAX_DELAY);
    if (!engines_initialized) {
        engines_epoch_us = engine_now_us();
        esp_err_t ret = engine_register_locked("cpu", "compute");
        if (ret == ESP_OK) {
#if SOLAR_OS_BOARD_HAS_SIMD
            ret = engine_register_locked("simd", "vector");
#endif
        }
        engines_initialized = ret == ESP_OK;
        xSemaphoreGive(engines_mutex);
        return ret;
    }
    xSemaphoreGive(engines_mutex);
    return ESP_OK;
}

static void engine_snapshot_locked(const engine_slot_t *slot,
                                   int64_t now,
                                   solar_os_engine_stats_t *stats)
{
    *stats = (solar_os_engine_stats_t) {0};
    strlcpy(stats->name, slot->name, sizeof(stats->name));
    strlcpy(stats->class_name, slot->class_name, sizeof(stats->class_name));
    strlcpy(stats->owner, slot->owner, sizeof(stats->owner));
    strlcpy(stats->label, slot->label, sizeof(stats->label));
    stats->active = slot->active_count > 0;
    stats->active_count = slot->active_count;
    stats->op_count = slot->op_count;
    stats->unit_count = slot->unit_count;
    stats->work_us = slot->work_us;
    stats->busy_us = slot->busy_us;
    if (slot->active_count > 0) {
        stats->busy_us += elapsed_us(slot->active_started_us, now);
    }
    stats->max_us = slot->max_us;
    stats->last_us = slot->last_us;
    stats->last_units = slot->last_units;
    stats->since_us = elapsed_us(engines_epoch_us, now);
}

esp_err_t solar_os_engines_init(void)
{
    return engines_ensure_init();
}

esp_err_t solar_os_engine_register(const char *name, const char *class_name)
{
    const esp_err_t init_ret = engines_ensure_init();
    if (init_ret != ESP_OK) {
        return init_ret;
    }

    xSemaphoreTake(engines_mutex, portMAX_DELAY);
    const esp_err_t ret = engine_register_locked(name, class_name);
    xSemaphoreGive(engines_mutex);
    return ret;
}

size_t solar_os_engine_count(void)
{
    size_t count = 0;

    if (engines_ensure_init() != ESP_OK) {
        return 0;
    }

    xSemaphoreTake(engines_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_ENGINE_MAX; i++) {
        if (engines[i].registered) {
            count++;
        }
    }
    xSemaphoreGive(engines_mutex);
    return count;
}

bool solar_os_engine_get(size_t index, solar_os_engine_stats_t *stats)
{
    size_t current = 0;

    if (stats == NULL || engines_ensure_init() != ESP_OK) {
        return false;
    }

    xSemaphoreTake(engines_mutex, portMAX_DELAY);
    const int64_t now = engine_now_us();
    for (size_t i = 0; i < SOLAR_OS_ENGINE_MAX; i++) {
        if (!engines[i].registered) {
            continue;
        }
        if (current++ == index) {
            engine_snapshot_locked(&engines[i], now, stats);
            xSemaphoreGive(engines_mutex);
            return true;
        }
    }
    xSemaphoreGive(engines_mutex);
    return false;
}

bool solar_os_engine_get_by_name(const char *name, solar_os_engine_stats_t *stats)
{
    if (stats == NULL || engines_ensure_init() != ESP_OK) {
        return false;
    }

    xSemaphoreTake(engines_mutex, portMAX_DELAY);
    const int slot = engine_find_locked(name);
    if (slot < 0) {
        xSemaphoreGive(engines_mutex);
        return false;
    }
    engine_snapshot_locked(&engines[slot], engine_now_us(), stats);
    xSemaphoreGive(engines_mutex);
    return true;
}

esp_err_t solar_os_engine_begin(const char *name,
                                const char *owner,
                                const char *label,
                                solar_os_engine_token_t *token)
{
    if (token == NULL || owner == NULL || owner[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t init_ret = engines_ensure_init();
    if (init_ret != ESP_OK) {
        return init_ret;
    }

    token->active = false;
    xSemaphoreTake(engines_mutex, portMAX_DELAY);
    const int slot_index = engine_find_locked(name);
    if (slot_index < 0) {
        xSemaphoreGive(engines_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    engine_slot_t *slot = &engines[slot_index];
    const int64_t now = engine_now_us();
    if (slot->active_count == 0) {
        slot->active_started_us = now;
    }
    slot->active_count++;
    strlcpy(slot->owner, owner, sizeof(slot->owner));
    strlcpy(slot->label, label != NULL ? label : "", sizeof(slot->label));

    token->active = true;
    token->slot = (size_t)slot_index;
    token->started_us = now;
    token->generation = slot->generation;
    xSemaphoreGive(engines_mutex);
    return ESP_OK;
}

esp_err_t solar_os_engine_end(solar_os_engine_token_t *token, uint64_t units)
{
    if (token == NULL || !token->active) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t init_ret = engines_ensure_init();
    if (init_ret != ESP_OK) {
        return init_ret;
    }

    xSemaphoreTake(engines_mutex, portMAX_DELAY);
    if (token->slot >= SOLAR_OS_ENGINE_MAX || !engines[token->slot].registered) {
        xSemaphoreGive(engines_mutex);
        token->active = false;
        return ESP_ERR_NOT_FOUND;
    }

    engine_slot_t *slot = &engines[token->slot];
    const int64_t now = engine_now_us();
    if (token->generation == slot->generation) {
        const uint64_t duration = elapsed_us(token->started_us, now);
        slot->op_count++;
        slot->unit_count += units;
        slot->work_us += duration;
        slot->last_us = duration;
        slot->last_units = units;
        if (duration > slot->max_us) {
            slot->max_us = duration;
        }
    }

    if (slot->active_count > 0) {
        slot->active_count--;
        if (slot->active_count == 0) {
            slot->busy_us += elapsed_us(slot->active_started_us, now);
            slot->active_started_us = 0;
        }
    }

    token->active = false;
    xSemaphoreGive(engines_mutex);
    return ESP_OK;
}

esp_err_t solar_os_engine_record(const char *name,
                                 const char *owner,
                                 const char *label,
                                 uint64_t duration_us,
                                 uint64_t units)
{
    if (owner == NULL || owner[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t init_ret = engines_ensure_init();
    if (init_ret != ESP_OK) {
        return init_ret;
    }

    xSemaphoreTake(engines_mutex, portMAX_DELAY);
    const int slot_index = engine_find_locked(name);
    if (slot_index < 0) {
        xSemaphoreGive(engines_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    engine_slot_t *slot = &engines[slot_index];
    slot->op_count++;
    slot->unit_count += units;
    slot->work_us += duration_us;
    slot->busy_us += duration_us;
    slot->last_us = duration_us;
    slot->last_units = units;
    if (duration_us > slot->max_us) {
        slot->max_us = duration_us;
    }
    strlcpy(slot->owner, owner, sizeof(slot->owner));
    strlcpy(slot->label, label != NULL ? label : "", sizeof(slot->label));
    xSemaphoreGive(engines_mutex);
    return ESP_OK;
}

void solar_os_engine_reset_all(void)
{
    if (engines_ensure_init() != ESP_OK) {
        return;
    }

    xSemaphoreTake(engines_mutex, portMAX_DELAY);
    const int64_t now = engine_now_us();
    engines_epoch_us = now;
    for (size_t i = 0; i < SOLAR_OS_ENGINE_MAX; i++) {
        if (!engines[i].registered) {
            continue;
        }
        engines[i].owner[0] = '\0';
        engines[i].label[0] = '\0';
        engines[i].op_count = 0;
        engines[i].unit_count = 0;
        engines[i].work_us = 0;
        engines[i].busy_us = 0;
        engines[i].max_us = 0;
        engines[i].last_us = 0;
        engines[i].last_units = 0;
        engines[i].generation++;
        if (engines[i].active_count > 0) {
            engines[i].active_started_us = now;
        }
    }
    xSemaphoreGive(engines_mutex);
}
