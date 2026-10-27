#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SOLAR_OS_SHUTDOWN_WAKE,
    SOLAR_OS_SHUTDOWN_PORTS,
    SOLAR_OS_SHUTDOWN_APPS,
    SOLAR_OS_SHUTDOWN_JOBS,
    SOLAR_OS_SHUTDOWN_RADIOS,
    SOLAR_OS_SHUTDOWN_STORAGE,
    SOLAR_OS_SHUTDOWN_DISPLAY,
    SOLAR_OS_SHUTDOWN_POWER,
    SOLAR_OS_SHUTDOWN_COMPLETE,
} solar_os_shutdown_stage_t;

typedef esp_err_t (*solar_os_shutdown_step_t)(solar_os_shutdown_stage_t stage,
                                              void *user);

/* Requests may come from any shell/task. The runtime owner polls the stages.
 * Successful shutdown never resumes a session. A failed cleanup stays stopped
 * and never advances to power-down. */
esp_err_t solar_os_shutdown_request(void);
bool solar_os_shutdown_requested(void);
void solar_os_shutdown_note_failure(esp_err_t error);
void solar_os_shutdown_poll(solar_os_shutdown_step_t step, void *user, uint32_t now_ms);
esp_err_t solar_os_shutdown_error(void);
solar_os_shutdown_stage_t solar_os_shutdown_stage(void);
const char *solar_os_shutdown_stage_name(solar_os_shutdown_stage_t stage);
