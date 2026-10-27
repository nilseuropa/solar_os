#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_POWER_PROFILE_NAME_MAX 16

typedef esp_err_t (*solar_os_power_off_fn_t)(void *user);
/* Only the provider named by the board manifest may cut the system supply. */
esp_err_t solar_os_power_register_off_provider(const char *name,
                                               solar_os_power_off_fn_t off,
                                               void *user);
void solar_os_power_unregister_off_provider(void *user);
esp_err_t solar_os_power_off(void);

typedef enum {
    SOLAR_OS_POWER_PROFILE_PERFORMANCE = 0,
    SOLAR_OS_POWER_PROFILE_BALANCED = 1,
    SOLAR_OS_POWER_PROFILE_BATTERY = 2,
    SOLAR_OS_POWER_PROFILE_LOWPOWER = 3,
} solar_os_power_profile_t;

typedef enum {
    SOLAR_OS_POWER_KEY_ACTION_OFF,
    SOLAR_OS_POWER_KEY_ACTION_SLEEP,
    SOLAR_OS_POWER_KEY_ACTION_SUSPEND,
    SOLAR_OS_POWER_KEY_ACTION_DEEPSLEEP,
    SOLAR_OS_POWER_KEY_ACTION_BLE_PAIR,
    SOLAR_OS_POWER_KEY_ACTION_ENTER,
    SOLAR_OS_POWER_KEY_ACTION_APP_EXIT,
    SOLAR_OS_POWER_KEY_ACTION_DEFAULT,
} solar_os_power_key_action_t;

typedef struct {
    bool initialized;
    solar_os_power_profile_t profile;
    solar_os_power_profile_t effective_profile;
    solar_os_power_key_action_t key_action;
    solar_os_power_key_action_t key_long_action;
    uint32_t idle_sleep_ms;
    uint32_t cpu_min_mhz;
    uint32_t cpu_max_mhz;
    bool automatic_light_sleep;
    bool explicit_sleep_active;
    bool suspend_active;
    uint32_t automatic_light_sleep_holdoff_ms;
    bool pm_configured;
    esp_err_t pm_last_error;
    bool bt_sleep_enabled;
    esp_err_t bt_sleep_last_error;
    uint32_t light_sleep_count;
    uint32_t last_activity_ms;
    uint32_t last_sleep_duration_ms;
    int last_wakeup_cause;
    uint64_t last_wakeup_ext1;
} solar_os_power_status_t;

esp_err_t solar_os_power_init(void);
void solar_os_power_get_status(solar_os_power_status_t *status);
esp_err_t solar_os_power_set_profile(solar_os_power_profile_t profile);
esp_err_t solar_os_power_apply_runtime_policy(void);
esp_err_t solar_os_power_begin_explicit_sleep(void);
esp_err_t solar_os_power_end_explicit_sleep(void);
esp_err_t solar_os_power_begin_suspend(void);
esp_err_t solar_os_power_end_suspend(void);
esp_err_t solar_os_power_hold_automatic_light_sleep(uint32_t duration_ms);
void solar_os_power_poll(void);
esp_err_t solar_os_power_set_idle_sleep_ms(uint32_t idle_sleep_ms);
esp_err_t solar_os_power_set_key_action(solar_os_power_key_action_t action);
esp_err_t solar_os_power_set_key_long_action(solar_os_power_key_action_t action);
void solar_os_power_note_activity(uint32_t now_ms);
bool solar_os_power_should_idle_sleep(uint32_t now_ms);
void solar_os_power_note_sleep_enter(uint32_t now_ms);
void solar_os_power_note_sleep_exit(uint32_t now_ms,
                                    int wakeup_cause,
                                    uint64_t wakeup_ext1,
                                    bool slept);
const char *solar_os_power_profile_name(solar_os_power_profile_t profile);
bool solar_os_power_parse_profile(const char *name, solar_os_power_profile_t *profile);
const char *solar_os_power_key_action_name(solar_os_power_key_action_t action);
bool solar_os_power_parse_key_action(const char *name, solar_os_power_key_action_t *action);
