#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_POWER_PROFILE_NAME_MAX 16

typedef enum {
    SOLAR_OS_POWER_PROFILE_PERFORMANCE,
    SOLAR_OS_POWER_PROFILE_BALANCED,
    SOLAR_OS_POWER_PROFILE_SOLAR,
    SOLAR_OS_POWER_PROFILE_OFFLINE,
} solar_os_power_profile_t;

typedef struct {
    bool initialized;
    solar_os_power_profile_t profile;
    uint32_t idle_sleep_ms;
    uint32_t light_sleep_count;
    uint32_t last_activity_ms;
    uint32_t last_sleep_duration_ms;
    int last_wakeup_cause;
    uint64_t last_wakeup_ext1;
} solar_os_power_status_t;

esp_err_t solar_os_power_init(void);
void solar_os_power_get_status(solar_os_power_status_t *status);
esp_err_t solar_os_power_set_profile(solar_os_power_profile_t profile);
esp_err_t solar_os_power_set_idle_sleep_ms(uint32_t idle_sleep_ms);
void solar_os_power_note_activity(uint32_t now_ms);
bool solar_os_power_should_idle_sleep(uint32_t now_ms);
void solar_os_power_note_sleep_enter(uint32_t now_ms);
void solar_os_power_note_sleep_exit(uint32_t now_ms,
                                    int wakeup_cause,
                                    uint64_t wakeup_ext1,
                                    bool slept);
const char *solar_os_power_profile_name(solar_os_power_profile_t profile);
bool solar_os_power_parse_profile(const char *name, solar_os_power_profile_t *profile);
