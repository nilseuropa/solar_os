#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../../src/services/solar_os_power.c"

static uint8_t saved[3], pending[3];
static bool present[3], pending_present[3], fail_save;
static unsigned commits;

static size_t key_index(const char *key)
{
    if (strcmp(key, "profile") == 0) return 0U;
    if (strcmp(key, "key") == 0) return 1U;
    assert(strcmp(key, "key_long") == 0);
    return 2U;
}
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "power") == 0);
    *handle = 1U;
    if (mode == NVS_READWRITE) {
        memcpy(pending, saved, sizeof(saved));
        memcpy(pending_present, present, sizeof(present));
    }
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    (void)handle;
    const size_t index = key_index(key);
    if (!present[index]) return ESP_ERR_NOT_FOUND;
    *value = saved[index];
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle;
    const size_t index = key_index(key);
    pending[index] = value;
    pending_present[index] = true;
    return ESP_OK;
}
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *value)
{
    (void)handle; (void)key; (void)value;
    return ESP_ERR_NOT_FOUND;
}
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    (void)handle; (void)key; (void)value;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    if (fail_save) return ESP_FAIL;
    memcpy(saved, pending, sizeof(saved));
    memcpy(present, pending_present, sizeof(present));
    commits++;
    return ESP_OK;
}
int64_t esp_timer_get_time(void) { return 1000000; }
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{
    (void)level; (void)tag; (void)fmt;
    return ESP_OK;
}
esp_err_t solar_os_backlight_set_suspended(bool suspended)
{
    (void)suspended;
    return ESP_OK;
}

static solar_os_power_status_t boot(void)
{
    power_status = (solar_os_power_status_t){0};
    assert(solar_os_power_init() == ESP_OK);
    solar_os_power_status_t status;
    solar_os_power_get_status(&status);
    return status;
}

int main(void)
{
#ifdef SOLAR_OS_BOARD_KEY_SHORT_INPUT_KEY
    const solar_os_power_key_action_t short_default =
        SOLAR_OS_BOARD_KEY_SHORT_INPUT_KEY == SOLAR_OS_KEY_ENTER ?
        SOLAR_OS_POWER_KEY_ACTION_ENTER :
        SOLAR_OS_BOARD_KEY_SHORT_INPUT_KEY == SOLAR_OS_KEY_APP_EXIT ?
        SOLAR_OS_POWER_KEY_ACTION_APP_EXIT : SOLAR_OS_POWER_KEY_ACTION_DEFAULT;
#else
    const solar_os_power_key_action_t short_default = SOLAR_OS_POWER_KEY_ACTION_SUSPEND;
#endif
    const solar_os_power_key_action_t long_default = SOLAR_OS_BOARD_HAS_WIRED_KEYBOARD ?
        SOLAR_OS_POWER_KEY_ACTION_SUSPEND : SOLAR_OS_POWER_KEY_ACTION_BLE_PAIR;
    solar_os_power_status_t status = boot();
    assert(status.key_action == short_default && status.key_long_action == long_default);
    assert(commits == 0U);
    /* Old persisted enum values retain their meanings on every board. */
    for (unsigned legacy = 0U; legacy <= 2U; legacy++) {
        saved[1] = legacy;
        present[1] = true;
        status = boot();
        assert((unsigned)status.key_action == legacy);
        assert(status.key_long_action == long_default);
    }
    assert(solar_os_power_set_key_action(SOLAR_OS_POWER_KEY_ACTION_ENTER) == ESP_OK);
    assert(solar_os_power_set_key_long_action(SOLAR_OS_POWER_KEY_ACTION_DEEPSLEEP) == ESP_OK);
    status = boot();
    assert(status.key_action == SOLAR_OS_POWER_KEY_ACTION_ENTER);
    assert(status.key_long_action == SOLAR_OS_POWER_KEY_ACTION_DEEPSLEEP);
    /* Saving another power setting must retain both independent KEY choices. */
    assert(solar_os_power_set_idle_sleep_ms(10000U) == ESP_OK);
    status = boot();
    assert(status.key_action == SOLAR_OS_POWER_KEY_ACTION_ENTER);
    assert(status.key_long_action == SOLAR_OS_POWER_KEY_ACTION_DEEPSLEEP);
    fail_save = true;
    assert(solar_os_power_set_key_action(SOLAR_OS_POWER_KEY_ACTION_SLEEP) == ESP_FAIL);
    assert(solar_os_power_set_key_long_action(SOLAR_OS_POWER_KEY_ACTION_BLE_PAIR) == ESP_FAIL);
    solar_os_power_get_status(&status);
    assert(status.key_action == SOLAR_OS_POWER_KEY_ACTION_ENTER);
    assert(status.key_long_action == SOLAR_OS_POWER_KEY_ACTION_DEEPSLEEP);
    status = boot();
    assert(status.key_action == SOLAR_OS_POWER_KEY_ACTION_ENTER);
    assert(status.key_long_action == SOLAR_OS_POWER_KEY_ACTION_DEEPSLEEP);
    fail_save = false;
    assert(solar_os_power_set_key_action(SOLAR_OS_POWER_KEY_ACTION_DEFAULT) == ESP_OK);
    assert(solar_os_power_set_key_long_action(SOLAR_OS_POWER_KEY_ACTION_DEFAULT) == ESP_OK);
    status = boot();
    assert(status.key_action == short_default && status.key_long_action == long_default);
    saved[1] = saved[2] = 255U;
    status = boot();
    assert(status.key_action == short_default && status.key_long_action == long_default);
    for (int action = SOLAR_OS_POWER_KEY_ACTION_OFF;
         action <= SOLAR_OS_POWER_KEY_ACTION_DEFAULT; action++) {
        solar_os_power_key_action_t parsed;
        assert(solar_os_power_parse_key_action(solar_os_power_key_action_name(action), &parsed));
        assert((int)parsed == action);
    }
    assert(!solar_os_power_parse_key_action("invalid", NULL));
    assert(!solar_os_power_parse_key_action(NULL, NULL));
    assert(solar_os_power_set_key_long_action(255) == ESP_ERR_INVALID_ARG);
    puts("power KEY settings tests passed");
    return 0;
}
