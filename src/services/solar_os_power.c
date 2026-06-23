#include "solar_os_power.h"

#include <string.h>

#include "nvs.h"
#include "solar_os_log.h"

#define POWER_NVS_NAMESPACE "power"
#define POWER_NVS_PROFILE_KEY "profile"
#define POWER_NVS_IDLE_KEY "idle_ms"

static const char *TAG = "solar_os_power";

static solar_os_power_status_t power_status = {
    .profile = SOLAR_OS_POWER_PROFILE_BALANCED,
};
static uint32_t sleep_enter_ms;

static uint32_t profile_default_idle_ms(solar_os_power_profile_t profile)
{
    switch (profile) {
    case SOLAR_OS_POWER_PROFILE_PERFORMANCE:
    case SOLAR_OS_POWER_PROFILE_BALANCED:
        return 0;
    case SOLAR_OS_POWER_PROFILE_SOLAR:
        return 300000;
    case SOLAR_OS_POWER_PROFILE_OFFLINE:
        return 60000;
    default:
        return 0;
    }
}

static bool profile_valid(solar_os_power_profile_t profile)
{
    return profile >= SOLAR_OS_POWER_PROFILE_PERFORMANCE &&
        profile <= SOLAR_OS_POWER_PROFILE_OFFLINE;
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

    power_status.profile = SOLAR_OS_POWER_PROFILE_BALANCED;
    power_status.idle_sleep_ms = profile_default_idle_ms(power_status.profile);
    power_status.initialized = true;

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(POWER_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGI(TAG, "using default power profile");
        return ESP_OK;
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
    nvs_close(nvs);

    SOLAR_OS_LOGI(TAG,
                  "profile=%s idle=%u ms",
                  solar_os_power_profile_name(power_status.profile),
                  (unsigned)power_status.idle_sleep_ms);
    return ESP_OK;
}

void solar_os_power_get_status(solar_os_power_status_t *status)
{
    if (status != NULL) {
        *status = power_status;
    }
}

esp_err_t solar_os_power_set_profile(solar_os_power_profile_t profile)
{
    if (!profile_valid(profile)) {
        return ESP_ERR_INVALID_ARG;
    }

    power_status.profile = profile;
    power_status.idle_sleep_ms = profile_default_idle_ms(profile);
    const esp_err_t ret = power_save();
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

void solar_os_power_note_activity(uint32_t now_ms)
{
    power_status.last_activity_ms = now_ms;
}

bool solar_os_power_should_idle_sleep(uint32_t now_ms)
{
    if (!power_status.initialized ||
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
    case SOLAR_OS_POWER_PROFILE_SOLAR:
        return "solar";
    case SOLAR_OS_POWER_PROFILE_OFFLINE:
        return "offline";
    default:
        return "unknown";
    }
}

bool solar_os_power_parse_profile(const char *name, solar_os_power_profile_t *profile)
{
    static const solar_os_power_profile_t profiles[] = {
        SOLAR_OS_POWER_PROFILE_PERFORMANCE,
        SOLAR_OS_POWER_PROFILE_BALANCED,
        SOLAR_OS_POWER_PROFILE_SOLAR,
        SOLAR_OS_POWER_PROFILE_OFFLINE,
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
