#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_engines_abi.h"

typedef struct {
    bool active;
    size_t slot;
    int64_t started_us;
    uint32_t generation;
} solar_os_engine_token_t;

esp_err_t solar_os_engines_init(void);
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
