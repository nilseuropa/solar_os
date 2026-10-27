#include "solar_os_backlight.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "solar_os_board.h"
#include "solar_os_log.h"

#define BACKLIGHT_PROVIDER_MAX 8U
#define BACKLIGHT_FOLLOW 101U
#define BACKLIGHT_DEFAULT 50U
#ifndef SOLAR_OS_BOARD_BACKLIGHT_ENABLED_ON_BOOT
#define SOLAR_OS_BOARD_BACKLIGHT_ENABLED_ON_BOOT 1
#endif

typedef struct {
    solar_os_backlight_set_fn_t set;
    void *user;
    uint8_t applied;
} backlight_provider_t;

static backlight_provider_t providers[BACKLIGHT_PROVIDER_MAX];
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
static bool initialized, suspended, display_available;
static bool output_disabled = !SOLAR_OS_BOARD_BACKLIGHT_ENABLED_ON_BOOT;
static uint8_t setting = BACKLIGHT_DEFAULT, display_percent;
static esp_err_t last_error;

static void take_mutex(void)
{
    portENTER_CRITICAL(&init_lock);
    if (mutex == NULL) mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    portEXIT_CRITICAL(&init_lock);
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!initialized) {
        nvs_handle_t nvs;
        uint8_t stored;
        if (nvs_open("backlight", NVS_READONLY, &nvs) == ESP_OK) {
            if (nvs_get_u8(nvs, "level", &stored) == ESP_OK && stored <= BACKLIGHT_FOLLOW)
                setting = stored;
            nvs_close(nvs);
        }
        initialized = true;
    }
}

static uint8_t requested_percent(uint8_t value)
{
    return value == BACKLIGHT_FOLLOW ? display_percent : value;
}

static uint8_t effective_percent(uint8_t value, bool off)
{
    return off || output_disabled ? 0U : requested_percent(value);
}

static void restore_locked(size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (providers[i].set == NULL) continue;
        const esp_err_t err = providers[i].set(providers[i].user, providers[i].applied);
        if (err != ESP_OK)
            SOLAR_OS_LOGW("backlight", "restore failed: %s", esp_err_to_name(err));
    }
}

/* Roll back every attempted provider, including a callback that partly failed. */
static esp_err_t apply_locked(uint8_t percent)
{
    for (size_t i = 0; i < BACKLIGHT_PROVIDER_MAX; i++) {
        if (providers[i].set == NULL) continue;
        const esp_err_t err = providers[i].set(providers[i].user, percent);
        if (err != ESP_OK) {
            restore_locked(i + 1U);
            last_error = err;
            return err;
        }
    }
    last_error = ESP_OK;
    return ESP_OK;
}

static void record_applied(uint8_t percent)
{
    for (size_t i = 0; i < BACKLIGHT_PROVIDER_MAX; i++)
        if (providers[i].set != NULL) providers[i].applied = percent;
}

esp_err_t solar_os_backlight_register(solar_os_backlight_set_fn_t set, void *user)
{
    if (set == NULL || user == NULL) return ESP_ERR_INVALID_ARG;
    take_mutex();
    size_t slot = BACKLIGHT_PROVIDER_MAX;
    for (size_t i = 0; i < BACKLIGHT_PROVIDER_MAX; i++) {
        if (providers[i].user == user) {
            xSemaphoreGive(mutex);
            return ESP_ERR_INVALID_STATE;
        }
        if (providers[i].set == NULL && slot == BACKLIGHT_PROVIDER_MAX) slot = i;
    }
    esp_err_t err = ESP_ERR_NO_MEM;
    if (slot < BACKLIGHT_PROVIDER_MAX) {
        const uint8_t percent = effective_percent(setting, suspended);
        err = set(user, percent);
        if (err == ESP_OK)
            providers[slot] = (backlight_provider_t){.set = set, .user = user, .applied = percent};
        else
            (void)set(user, 0U);
    }
    xSemaphoreGive(mutex);
    return err;
}

void solar_os_backlight_unregister(void *user)
{
    if (user == NULL) return;
    take_mutex();
    for (size_t i = 0; i < BACKLIGHT_PROVIDER_MAX; i++) {
        if (providers[i].set == NULL || providers[i].user != user) continue;
        const esp_err_t err = providers[i].set(user, 0U);
        if (err != ESP_OK) SOLAR_OS_LOGW("backlight", "stop failed: %s", esp_err_to_name(err));
        memset(&providers[i], 0, sizeof(providers[i]));
    }
    xSemaphoreGive(mutex);
}

esp_err_t solar_os_backlight_get_status(solar_os_backlight_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    take_mutex();
    *status = (solar_os_backlight_status_t){
        .follow_display = setting == BACKLIGHT_FOLLOW, .suspended = suspended,
        .percent = requested_percent(setting),
        .effective_percent = effective_percent(setting, suspended), .last_error = last_error,
    };
    for (size_t i = 0; i < BACKLIGHT_PROVIDER_MAX; i++) {
        if (providers[i].set == NULL) continue;
        if (status->providers == 0U) status->effective_percent = providers[i].applied;
        status->providers++;
    }
    xSemaphoreGive(mutex);
    return status->providers != 0U ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t set_setting(uint8_t value)
{
    take_mutex();
    bool supported = false;
    for (size_t i = 0; i < BACKLIGHT_PROVIDER_MAX; i++) supported |= providers[i].set != NULL;
    esp_err_t err = !supported || (value == BACKLIGHT_FOLLOW && !display_available) ?
        ESP_ERR_NOT_SUPPORTED : ESP_OK;
    /* An explicit lighting command enables output for this boot. Loading a
     * saved brightness, display changes and resume leave the boot gate alone. */
    const uint8_t percent = suspended ? 0U : requested_percent(value);
    if (err == ESP_OK) err = apply_locked(percent);
    if (err == ESP_OK) {
        nvs_handle_t nvs;
        err = nvs_open("backlight", NVS_READWRITE, &nvs);
        if (err == ESP_OK) {
            err = nvs_set_u8(nvs, "level", value);
            if (err == ESP_OK) err = nvs_commit(nvs);
            nvs_close(nvs);
        }
        if (err == ESP_OK) {
            setting = value;
            output_disabled = false;
            record_applied(percent);
        } else {
            restore_locked(BACKLIGHT_PROVIDER_MAX);
            last_error = err;
        }
    }
    xSemaphoreGive(mutex);
    return err;
}

esp_err_t solar_os_backlight_set_brightness(uint8_t percent)
{
    return percent <= 100U ? set_setting(percent) : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_backlight_set_follow(void) { return set_setting(BACKLIGHT_FOLLOW); }

esp_err_t solar_os_backlight_toggle(void)
{
    take_mutex();
    bool supported = false;
    for (size_t i = 0; i < BACKLIGHT_PROVIDER_MAX; i++) supported |= providers[i].set != NULL;
    /* Toggle the user's awake state even while sleep temporarily blanks it.
     * Keep this choice in RAM so a shortcut never changes the saved level. */
    const bool disable = !output_disabled && requested_percent(setting) != 0U;
    const uint8_t value = !disable && requested_percent(setting) == 0U ?
        BACKLIGHT_DEFAULT : setting;
    const uint8_t percent = suspended || disable ? 0U : requested_percent(value);
    const esp_err_t err = supported ? apply_locked(percent) : ESP_ERR_NOT_SUPPORTED;
    if (err == ESP_OK) {
        setting = value;
        output_disabled = disable;
        record_applied(percent);
    }
    xSemaphoreGive(mutex);
    return err;
}

esp_err_t solar_os_backlight_display_changed(uint8_t percent)
{
    if (percent > 100U) return ESP_ERR_INVALID_ARG;
    take_mutex();
    display_available = true;
    display_percent = percent;
    esp_err_t err = ESP_OK;
    if (setting == BACKLIGHT_FOLLOW) {
        const uint8_t effective = effective_percent(setting, suspended);
        err = apply_locked(effective);
        if (err == ESP_OK) record_applied(effective);
    }
    xSemaphoreGive(mutex);
    return err;
}

esp_err_t solar_os_backlight_set_suspended(bool off)
{
    take_mutex();
    esp_err_t err = ESP_OK;
    if (suspended != off) {
        const uint8_t percent = effective_percent(setting, off);
        err = apply_locked(percent);
        if (err == ESP_OK) { suspended = off; record_applied(percent); }
    }
    xSemaphoreGive(mutex);
    return err;
}
