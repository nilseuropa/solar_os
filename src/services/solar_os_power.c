#include "solar_os_power.h"
#include "solar_os_backlight.h"

#include <string.h>

#include "esp_bt.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "nvs.h"
#include "solar_os_board.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"

#define POWER_NVS_NAMESPACE "power"
#define POWER_NVS_PROFILE_KEY "profile"
#define POWER_NVS_IDLE_KEY "idle_ms"
#define POWER_NVS_KEY_ACTION_KEY "key"
#define POWER_NVS_KEY_LONG_ACTION_KEY "key_long"
#define POWER_CPU_PERFORMANCE_MHZ 240U
#define POWER_CPU_BALANCED_MHZ 160U
#define POWER_CPU_BATTERY_MHZ 160U
#define POWER_CPU_LOWPOWER_MHZ 80U

static const char *TAG = "solar_os_power";
static portMUX_TYPE off_lock = portMUX_INITIALIZER_UNLOCKED;
static solar_os_power_off_fn_t off_provider;
static void *off_user;

esp_err_t solar_os_power_register_off_provider(const char *name,
                                               solar_os_power_off_fn_t off,
                                               void *user)
{
    if (name == NULL || off == NULL || user == NULL) return ESP_ERR_INVALID_ARG;
#ifndef SOLAR_OS_BOARD_POWEROFF_PROVIDER
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (strcmp(name, SOLAR_OS_BOARD_POWEROFF_PROVIDER) != 0) return ESP_ERR_NOT_SUPPORTED;
    portENTER_CRITICAL(&off_lock);
    const bool busy = off_provider != NULL && off_user != user;
    if (!busy) { off_provider = off; off_user = user; }
    portEXIT_CRITICAL(&off_lock);
    return busy ? ESP_ERR_INVALID_STATE : ESP_OK;
#endif
}

void solar_os_power_unregister_off_provider(void *user)
{
    portENTER_CRITICAL(&off_lock);
    if (off_user == user) { off_provider = NULL; off_user = NULL; }
    portEXIT_CRITICAL(&off_lock);
}

esp_err_t solar_os_power_off(void)
{
    portENTER_CRITICAL(&off_lock);
    const solar_os_power_off_fn_t off = off_provider;
    void *user = off_user;
    portEXIT_CRITICAL(&off_lock);
    return off != NULL ? off(user) : ESP_ERR_NOT_SUPPORTED;
}

static solar_os_power_status_t power_status = {
    .profile = SOLAR_OS_POWER_PROFILE_PERFORMANCE,
    .effective_profile = SOLAR_OS_POWER_PROFILE_PERFORMANCE,
    .key_action = SOLAR_OS_POWER_KEY_ACTION_DEFAULT,
    .key_long_action = SOLAR_OS_POWER_KEY_ACTION_DEFAULT,
};
static uint32_t sleep_enter_ms;
static uint32_t automatic_light_sleep_holdoff_until_ms;

static uint32_t power_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool millis_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static uint32_t automatic_light_sleep_holdoff_remaining_ms(uint32_t now_ms)
{
    if (automatic_light_sleep_holdoff_until_ms == 0 ||
        !millis_before(now_ms, automatic_light_sleep_holdoff_until_ms)) {
        return 0;
    }

    return automatic_light_sleep_holdoff_until_ms - now_ms;
}

static void update_automatic_light_sleep_holdoff_status(uint32_t now_ms)
{
    const uint32_t remaining_ms = automatic_light_sleep_holdoff_remaining_ms(now_ms);
    if (remaining_ms == 0) {
        automatic_light_sleep_holdoff_until_ms = 0;
    }
    power_status.automatic_light_sleep_holdoff_ms = remaining_ms;
}

static uint32_t profile_default_idle_ms(solar_os_power_profile_t profile)
{
    switch (profile) {
    case SOLAR_OS_POWER_PROFILE_PERFORMANCE:
    case SOLAR_OS_POWER_PROFILE_BALANCED:
    case SOLAR_OS_POWER_PROFILE_BATTERY:
        return 0;
    case SOLAR_OS_POWER_PROFILE_LOWPOWER:
        return 60000;
    default:
        return 0;
    }
}

static void profile_pm_config(solar_os_power_profile_t profile,
                              uint32_t *min_mhz,
                              uint32_t *max_mhz,
                              bool *automatic_light_sleep)
{
    uint32_t min_value = POWER_CPU_BALANCED_MHZ;
    uint32_t max_value = POWER_CPU_BALANCED_MHZ;
    bool auto_sleep = false;

    switch (profile) {
    case SOLAR_OS_POWER_PROFILE_PERFORMANCE:
        min_value = POWER_CPU_PERFORMANCE_MHZ;
        max_value = POWER_CPU_PERFORMANCE_MHZ;
        break;
    case SOLAR_OS_POWER_PROFILE_BALANCED:
        min_value = POWER_CPU_BALANCED_MHZ;
        max_value = POWER_CPU_BALANCED_MHZ;
        break;
    case SOLAR_OS_POWER_PROFILE_BATTERY:
        min_value = POWER_CPU_BATTERY_MHZ;
        max_value = POWER_CPU_BATTERY_MHZ;
        auto_sleep = true;
        break;
    case SOLAR_OS_POWER_PROFILE_LOWPOWER:
        min_value = POWER_CPU_LOWPOWER_MHZ;
        max_value = POWER_CPU_LOWPOWER_MHZ;
        auto_sleep = true;
        break;
    default:
        break;
    }

    if (min_value < SOLAR_OS_BOARD_CPU_FLOOR_MHZ) {
        min_value = SOLAR_OS_BOARD_CPU_FLOOR_MHZ;
    }
    if (max_value < SOLAR_OS_BOARD_CPU_FLOOR_MHZ) {
        max_value = SOLAR_OS_BOARD_CPU_FLOOR_MHZ;
    }

    if (min_mhz != NULL) {
        *min_mhz = min_value;
    }
    if (max_mhz != NULL) {
        *max_mhz = max_value;
    }
    if (automatic_light_sleep != NULL) {
        *automatic_light_sleep = auto_sleep;
    }
}

static bool profile_valid(solar_os_power_profile_t profile)
{
    return profile >= SOLAR_OS_POWER_PROFILE_PERFORMANCE &&
        profile <= SOLAR_OS_POWER_PROFILE_LOWPOWER;
}

static bool key_action_valid(solar_os_power_key_action_t action)
{
    return action >= SOLAR_OS_POWER_KEY_ACTION_OFF &&
        action <= SOLAR_OS_POWER_KEY_ACTION_DEFAULT;
}

static solar_os_power_key_action_t default_key_action(bool long_press)
{
    if (long_press) {
#if SOLAR_OS_BOARD_HAS_WIRED_KEYBOARD
        return SOLAR_OS_POWER_KEY_ACTION_SUSPEND;
#else
        return SOLAR_OS_POWER_KEY_ACTION_BLE_PAIR;
#endif
    }
#ifdef SOLAR_OS_BOARD_KEY_SHORT_INPUT_KEY
    if (SOLAR_OS_BOARD_KEY_SHORT_INPUT_KEY == SOLAR_OS_KEY_ENTER) {
        return SOLAR_OS_POWER_KEY_ACTION_ENTER;
    }
    if (SOLAR_OS_BOARD_KEY_SHORT_INPUT_KEY == SOLAR_OS_KEY_APP_EXIT) {
        return SOLAR_OS_POWER_KEY_ACTION_APP_EXIT;
    }
    return SOLAR_OS_POWER_KEY_ACTION_DEFAULT;
#else
    return SOLAR_OS_POWER_KEY_ACTION_SUSPEND;
#endif
}

static esp_err_t apply_bt_sleep_policy(bool automatic_light_sleep)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_CTRL_MODEM_SLEEP
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        power_status.bt_sleep_enabled = false;
        power_status.bt_sleep_last_error = ESP_ERR_INVALID_STATE;
        return ESP_OK;
    }

    const esp_err_t ret = automatic_light_sleep ? esp_bt_sleep_enable() : esp_bt_sleep_disable();
    power_status.bt_sleep_enabled = automatic_light_sleep && ret == ESP_OK;
    power_status.bt_sleep_last_error = ret;
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED && ret != ESP_ERR_INVALID_STATE) {
        SOLAR_OS_LOGW(TAG,
                      "BT modem sleep %s failed: %s",
                      automatic_light_sleep ? "enable" : "disable",
                      esp_err_to_name(ret));
    }
    return ESP_OK;
#else
    power_status.bt_sleep_enabled = false;
    power_status.bt_sleep_last_error = ESP_ERR_NOT_SUPPORTED;
    (void)automatic_light_sleep;
    return ESP_OK;
#endif
}

esp_err_t solar_os_power_apply_runtime_policy(void)
{
    uint32_t min_mhz = 0;
    uint32_t max_mhz = 0;
    bool profile_automatic_light_sleep = false;
    power_status.effective_profile = power_status.suspend_active ?
        SOLAR_OS_POWER_PROFILE_LOWPOWER : power_status.profile;
    profile_pm_config(power_status.effective_profile,
                      &min_mhz,
                      &max_mhz,
                      &profile_automatic_light_sleep);
    update_automatic_light_sleep_holdoff_status(power_millis());

    power_status.cpu_min_mhz = min_mhz;
    power_status.cpu_max_mhz = max_mhz;

#if CONFIG_PM_ENABLE
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
    const bool effective_light_sleep =
        profile_automatic_light_sleep &&
        !power_status.explicit_sleep_active &&
        power_status.automatic_light_sleep_holdoff_ms == 0;
#else
    const bool effective_light_sleep = false;
#endif
    power_status.automatic_light_sleep = effective_light_sleep;
    esp_pm_config_t pm_config = {
        .max_freq_mhz = (int)max_mhz,
        .min_freq_mhz = (int)min_mhz,
        .light_sleep_enable = effective_light_sleep,
    };

    const esp_err_t pm_ret = esp_pm_configure(&pm_config);
    power_status.pm_configured = pm_ret == ESP_OK;
    power_status.pm_last_error = pm_ret;
    if (pm_ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG,
                      "PM profile apply failed: %s max=%u min=%u auto=%s",
                      esp_err_to_name(pm_ret),
                      (unsigned)max_mhz,
                      (unsigned)min_mhz,
                      effective_light_sleep ? "on" : "off");
        (void)apply_bt_sleep_policy(false);
        return pm_ret;
    }
#else
    power_status.automatic_light_sleep = false;
    power_status.pm_configured = false;
    power_status.pm_last_error = ESP_ERR_NOT_SUPPORTED;
#endif

    (void)apply_bt_sleep_policy(power_status.automatic_light_sleep);
    SOLAR_OS_LOGI(TAG,
                  "PM profile=%s max=%u min=%u auto-light=%s",
                  solar_os_power_profile_name(power_status.effective_profile),
                  (unsigned)max_mhz,
                  (unsigned)min_mhz,
                  power_status.automatic_light_sleep ? "on" :
                  power_status.automatic_light_sleep_holdoff_ms > 0 ? "holdoff" :
                  power_status.explicit_sleep_active && profile_automatic_light_sleep ? "suppressed" :
                  "off");
    return solar_os_backlight_set_suspended(
        power_status.suspend_active || power_status.explicit_sleep_active);
}

esp_err_t solar_os_power_begin_explicit_sleep(void)
{
    power_status.explicit_sleep_active = true;
    const esp_err_t err = solar_os_power_apply_runtime_policy();
    if (err != ESP_OK) {
        power_status.explicit_sleep_active = false;
        (void)solar_os_power_apply_runtime_policy();
    }
    return err;
}

esp_err_t solar_os_power_end_explicit_sleep(void)
{
    power_status.explicit_sleep_active = false;
    return solar_os_power_apply_runtime_policy();
}

esp_err_t solar_os_power_begin_suspend(void)
{
    if (power_status.suspend_active) {
        return ESP_ERR_INVALID_STATE;
    }

    power_status.suspend_active = true;
    const esp_err_t ret = solar_os_power_apply_runtime_policy();
    if (ret != ESP_OK) {
        power_status.suspend_active = false;
        (void)solar_os_power_apply_runtime_policy();
    }
    return ret;
}

esp_err_t solar_os_power_end_suspend(void)
{
    if (!power_status.suspend_active) {
        return ESP_ERR_INVALID_STATE;
    }

    power_status.suspend_active = false;
    return solar_os_power_apply_runtime_policy();
}

esp_err_t solar_os_power_hold_automatic_light_sleep(uint32_t duration_ms)
{
    if (duration_ms == 0) {
        automatic_light_sleep_holdoff_until_ms = 0;
    } else {
        automatic_light_sleep_holdoff_until_ms = power_millis() + duration_ms;
    }
    return solar_os_power_apply_runtime_policy();
}

void solar_os_power_poll(void)
{
    if (automatic_light_sleep_holdoff_until_ms == 0) {
        return;
    }

    update_automatic_light_sleep_holdoff_status(power_millis());
    if (power_status.automatic_light_sleep_holdoff_ms == 0) {
        (void)solar_os_power_apply_runtime_policy();
    }
}

static esp_err_t power_save(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(POWER_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, POWER_NVS_PROFILE_KEY, (uint8_t)power_status.profile);
    if (ret == ESP_OK) {
        ret = nvs_set_u32(nvs, POWER_NVS_IDLE_KEY, power_status.idle_sleep_ms);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, POWER_NVS_KEY_ACTION_KEY, (uint8_t)power_status.key_action);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, POWER_NVS_KEY_LONG_ACTION_KEY, (uint8_t)power_status.key_long_action);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

esp_err_t solar_os_power_init(void)
{
    if (power_status.initialized) {
        return ESP_OK;
    }

    power_status.profile = SOLAR_OS_POWER_PROFILE_PERFORMANCE;
    power_status.effective_profile = SOLAR_OS_POWER_PROFILE_PERFORMANCE;
    power_status.key_action = SOLAR_OS_POWER_KEY_ACTION_DEFAULT;
    power_status.key_long_action = SOLAR_OS_POWER_KEY_ACTION_DEFAULT;
    power_status.idle_sleep_ms = profile_default_idle_ms(power_status.profile);
    power_status.initialized = true;

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(POWER_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGI(TAG, "using default power profile");
        return solar_os_power_apply_runtime_policy();
    }

    uint8_t profile = (uint8_t)power_status.profile;
    if (nvs_get_u8(nvs, POWER_NVS_PROFILE_KEY, &profile) == ESP_OK &&
        profile_valid((solar_os_power_profile_t)profile)) {
        power_status.profile = (solar_os_power_profile_t)profile;
    }

    uint32_t idle_sleep_ms = power_status.idle_sleep_ms;
    if (nvs_get_u32(nvs, POWER_NVS_IDLE_KEY, &idle_sleep_ms) == ESP_OK) {
        power_status.idle_sleep_ms = idle_sleep_ms;
    }

    uint8_t key_action = (uint8_t)power_status.key_action;
    if (nvs_get_u8(nvs, POWER_NVS_KEY_ACTION_KEY, &key_action) == ESP_OK &&
        key_action_valid((solar_os_power_key_action_t)key_action)) {
        power_status.key_action = (solar_os_power_key_action_t)key_action;
    }
    uint8_t key_long_action = (uint8_t)power_status.key_long_action;
    if (nvs_get_u8(nvs, POWER_NVS_KEY_LONG_ACTION_KEY, &key_long_action) == ESP_OK &&
        key_action_valid((solar_os_power_key_action_t)key_long_action)) {
        power_status.key_long_action = (solar_os_power_key_action_t)key_long_action;
    }
    nvs_close(nvs);

    SOLAR_OS_LOGI(TAG,
                  "profile=%s idle=%u ms key=%s",
                  solar_os_power_profile_name(power_status.profile),
                  (unsigned)power_status.idle_sleep_ms,
                  solar_os_power_key_action_name(power_status.key_action));
    return solar_os_power_apply_runtime_policy();
}

void solar_os_power_get_status(solar_os_power_status_t *status)
{
    if (status != NULL) {
        update_automatic_light_sleep_holdoff_status(power_millis());
        *status = power_status;
        if (status->key_action == SOLAR_OS_POWER_KEY_ACTION_DEFAULT) {
            status->key_action = default_key_action(false);
        }
        if (status->key_long_action == SOLAR_OS_POWER_KEY_ACTION_DEFAULT) {
            status->key_long_action = default_key_action(true);
        }
    }
}

esp_err_t solar_os_power_set_profile(solar_os_power_profile_t profile)
{
    if (!profile_valid(profile)) {
        return ESP_ERR_INVALID_ARG;
    }

    power_status.profile = profile;
    power_status.idle_sleep_ms = profile_default_idle_ms(profile);
    esp_err_t ret = solar_os_power_apply_runtime_policy();
    if (ret == ESP_OK) {
        ret = power_save();
    }
    if (ret == ESP_OK) {
        SOLAR_OS_LOGI(TAG,
                      "profile=%s idle=%u ms",
                      solar_os_power_profile_name(profile),
                      (unsigned)power_status.idle_sleep_ms);
    }
    return ret;
}

esp_err_t solar_os_power_set_idle_sleep_ms(uint32_t idle_sleep_ms)
{
    power_status.idle_sleep_ms = idle_sleep_ms;
    const esp_err_t ret = power_save();
    if (ret == ESP_OK) {
        SOLAR_OS_LOGI(TAG, "idle sleep=%u ms", (unsigned)idle_sleep_ms);
    }
    return ret;
}

esp_err_t solar_os_power_set_key_action(solar_os_power_key_action_t action)
{
    if (!key_action_valid(action)) {
        return ESP_ERR_INVALID_ARG;
    }

    const solar_os_power_key_action_t previous = power_status.key_action;
    power_status.key_action = action;
    const esp_err_t ret = power_save();
    if (ret == ESP_OK) {
        SOLAR_OS_LOGI(TAG, "KEY action=%s", solar_os_power_key_action_name(action));
    } else {
        power_status.key_action = previous;
    }
    return ret;
}

esp_err_t solar_os_power_set_key_long_action(solar_os_power_key_action_t action)
{
    if (!key_action_valid(action)) {
        return ESP_ERR_INVALID_ARG;
    }
    const solar_os_power_key_action_t previous = power_status.key_long_action;
    power_status.key_long_action = action;
    const esp_err_t ret = power_save();
    if (ret != ESP_OK) {
        power_status.key_long_action = previous;
    }
    return ret;
}

void solar_os_power_note_activity(uint32_t now_ms)
{
    power_status.last_activity_ms = now_ms;
}

bool solar_os_power_should_idle_sleep(uint32_t now_ms)
{
    if (!power_status.initialized ||
        power_status.suspend_active ||
        power_status.idle_sleep_ms == 0 ||
        power_status.last_activity_ms == 0) {
        return false;
    }

    return (now_ms - power_status.last_activity_ms) >= power_status.idle_sleep_ms;
}

void solar_os_power_note_sleep_enter(uint32_t now_ms)
{
    sleep_enter_ms = now_ms;
}

void solar_os_power_note_sleep_exit(uint32_t now_ms,
                                    int wakeup_cause,
                                    uint64_t wakeup_ext1,
                                    bool slept)
{
    power_status.last_activity_ms = now_ms;
    power_status.last_wakeup_cause = wakeup_cause;
    power_status.last_wakeup_ext1 = wakeup_ext1;
    if (!slept) {
        return;
    }

    power_status.light_sleep_count++;
    power_status.last_sleep_duration_ms = sleep_enter_ms != 0 ? now_ms - sleep_enter_ms : 0;
}

const char *solar_os_power_profile_name(solar_os_power_profile_t profile)
{
    switch (profile) {
    case SOLAR_OS_POWER_PROFILE_PERFORMANCE:
        return "performance";
    case SOLAR_OS_POWER_PROFILE_BALANCED:
        return "balanced";
    case SOLAR_OS_POWER_PROFILE_BATTERY:
        return "battery";
    case SOLAR_OS_POWER_PROFILE_LOWPOWER:
        return "lowpower";
    default:
        return "unknown";
    }
}

bool solar_os_power_parse_profile(const char *name, solar_os_power_profile_t *profile)
{
    static const solar_os_power_profile_t profiles[] = {
        SOLAR_OS_POWER_PROFILE_PERFORMANCE,
        SOLAR_OS_POWER_PROFILE_BALANCED,
        SOLAR_OS_POWER_PROFILE_BATTERY,
        SOLAR_OS_POWER_PROFILE_LOWPOWER,
    };

    if (name == NULL) {
        return false;
    }

    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        if (strcmp(name, solar_os_power_profile_name(profiles[i])) == 0) {
            if (profile != NULL) {
                *profile = profiles[i];
            }
            return true;
        }
    }
    return false;
}

const char *solar_os_power_key_action_name(solar_os_power_key_action_t action)
{
    switch (action) {
    case SOLAR_OS_POWER_KEY_ACTION_OFF:
        return "off";
    case SOLAR_OS_POWER_KEY_ACTION_SLEEP:
        return "sleep";
    case SOLAR_OS_POWER_KEY_ACTION_SUSPEND:
        return "suspend";
    case SOLAR_OS_POWER_KEY_ACTION_DEEPSLEEP:
        return "deepsleep";
    case SOLAR_OS_POWER_KEY_ACTION_BLE_PAIR:
        return "ble-pair";
    case SOLAR_OS_POWER_KEY_ACTION_ENTER:
        return "enter";
    case SOLAR_OS_POWER_KEY_ACTION_APP_EXIT:
        return "app-exit";
    case SOLAR_OS_POWER_KEY_ACTION_DEFAULT:
        return "default";
    default:
        return "unknown";
    }
}

bool solar_os_power_parse_key_action(const char *name, solar_os_power_key_action_t *action)
{
    static const solar_os_power_key_action_t actions[] = {
        SOLAR_OS_POWER_KEY_ACTION_OFF,
        SOLAR_OS_POWER_KEY_ACTION_SLEEP,
        SOLAR_OS_POWER_KEY_ACTION_SUSPEND,
        SOLAR_OS_POWER_KEY_ACTION_DEEPSLEEP,
        SOLAR_OS_POWER_KEY_ACTION_BLE_PAIR,
        SOLAR_OS_POWER_KEY_ACTION_ENTER,
        SOLAR_OS_POWER_KEY_ACTION_APP_EXIT,
        SOLAR_OS_POWER_KEY_ACTION_DEFAULT,
    };

    if (name == NULL) {
        return false;
    }

    /* Preserve the pre-4.8.5 command spelling for startup scripts. */
    if (strcmp(name, "light") == 0) {
        if (action != NULL) {
            *action = SOLAR_OS_POWER_KEY_ACTION_SLEEP;
        }
        return true;
    }

    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
        if (strcmp(name, solar_os_power_key_action_name(actions[i])) == 0) {
            if (action != NULL) {
                *action = actions[i];
            }
            return true;
        }
    }
    return false;
}
