#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_ENGINE_NAME_MAX 16
#define SOLAR_OS_ENGINE_CLASS_MAX 16
#define SOLAR_OS_ENGINE_OWNER_MAX 24
#define SOLAR_OS_ENGINE_LABEL_MAX 24

typedef struct {
    bool active;
    size_t slot;
    int64_t started_us;
    uint32_t generation;
} solar_os_engine_token_t;

typedef struct {
    char name[SOLAR_OS_ENGINE_NAME_MAX];
    char class_name[SOLAR_OS_ENGINE_CLASS_MAX];
    char owner[SOLAR_OS_ENGINE_OWNER_MAX];
    char label[SOLAR_OS_ENGINE_LABEL_MAX];
    bool active;
    uint32_t active_count;
    uint64_t op_count;
    uint64_t unit_count;
    uint64_t work_us;
    uint64_t busy_us;
    uint64_t max_us;
    uint64_t last_us;
    uint64_t last_units;
    uint64_t since_us;
} solar_os_engine_stats_t;

esp_err_t solar_os_engines_init(void);
esp_err_t solar_os_engine_register(const char *name, const char *class_name);
size_t solar_os_engine_count(void);
bool solar_os_engine_get(size_t index, solar_os_engine_stats_t *stats);
bool solar_os_engine_get_by_name(const char *name, solar_os_engine_stats_t *stats);
esp_err_t solar_os_engine_begin(const char *name,
                                const char *owner,
                                const char *label,
                                solar_os_engine_token_t *token);
esp_err_t solar_os_engine_end(solar_os_engine_token_t *token, uint64_t units);
esp_err_t solar_os_engine_record(const char *name,
                                 const char *owner,
                                 const char *label,
                                 uint64_t duration_us,
                                 uint64_t units);
void solar_os_engine_reset_all(void);
