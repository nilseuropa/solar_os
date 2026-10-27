#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvs.h"
#include "solar_os_backlight.h"
#include "solar_os_log.h"

static int stored = -1, pending;
static bool fail_save;
static unsigned commits;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "backlight") == 0);
    if (mode == NVS_READONLY && stored < 0) return ESP_ERR_NVS_NOT_FOUND;
    *handle = 1;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    assert(handle == 1 && strcmp(key, "level") == 0);
    *value = (uint8_t)stored;
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle == 1 && strcmp(key, "level") == 0);
    pending = value;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    commits++;
    if (fail_save) return ESP_FAIL;
    stored = pending;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "error"; }
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{ (void)level; (void)tag; (void)fmt; return ESP_OK; }

typedef struct { uint8_t level; int fail_at; unsigned calls; } light_t;
static esp_err_t set_light(void *user, uint8_t percent)
{
    light_t *light = user;
    light->level = percent; /* Model a device that changes before reporting an error. */
    light->calls++;
    if (light->fail_at == percent) { light->fail_at = -1; return ESP_FAIL; }
    return ESP_OK;
}

int main(int argc, char **argv)
{
    if (argc > 1) stored = atoi(argv[1]);
    solar_os_backlight_status_t status;
    assert(solar_os_backlight_get_status(NULL) == ESP_ERR_INVALID_ARG);
    assert(solar_os_backlight_get_status(&status) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_backlight_set_brightness(10) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_backlight_set_brightness(101) == ESP_ERR_INVALID_ARG);
    assert(solar_os_backlight_register(NULL, &status) == ESP_ERR_INVALID_ARG);
    light_t first = {.fail_at = -1}, second = {.fail_at = -1};
    assert(solar_os_backlight_register(set_light, &first) == ESP_OK);
    assert(solar_os_backlight_register(set_light, &first) == ESP_ERR_INVALID_STATE);
    const bool boot_enabled = argc <= 2 || atoi(argv[2]) != 0;
    const int original_stored = stored;
    const unsigned configured = stored == 101 ? 0U : stored >= 0 && stored <= 100 ? (unsigned)stored : 50U;
    const unsigned initial = boot_enabled ? configured : 0U;
    assert(first.level == initial);
    assert(solar_os_backlight_get_status(&status) == ESP_OK);
    assert(status.percent == configured && status.effective_percent == initial);
    assert(stored == original_stored && commits == 0);
    assert(solar_os_backlight_set_follow() == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_backlight_display_changed(80) == ESP_OK);
    const unsigned after_display = boot_enabled && stored == 101 ? 80U : initial;
    assert(first.level == after_display);
    assert(solar_os_backlight_set_suspended(true) == ESP_OK && first.level == 0);
    assert(solar_os_backlight_set_suspended(false) == ESP_OK && first.level == after_display);
    assert(stored == original_stored && commits == 0);
    if (!boot_enabled) {
        /* A failed explicit command must leave the boot-disabled output and
         * the saved brightness intact. */
        fail_save = true;
        assert(solar_os_backlight_set_brightness(45) == ESP_FAIL);
        assert(first.level == 0 && stored == original_stored);
        fail_save = false;
        assert(solar_os_backlight_display_changed(80) == ESP_OK && first.level == 0);
        assert(solar_os_backlight_get_status(&status) == ESP_OK);
        assert(status.percent == (stored == 101 ? 80U : configured));
    }
    assert(solar_os_backlight_set_brightness(25) == ESP_OK && stored == 25);
    assert(solar_os_backlight_register(set_light, &second) == ESP_OK && second.level == 25);
    assert(solar_os_backlight_get_status(&status) == ESP_OK && status.providers == 2);
    assert(solar_os_backlight_set_follow() == ESP_OK && stored == 101);
    assert(first.level == 80 && second.level == 80);
    const unsigned saves = commits;
    assert(solar_os_backlight_display_changed(35) == ESP_OK);
    assert(first.level == 35 && second.level == 35 && commits == saves);
    second.fail_at = 40;
    assert(solar_os_backlight_display_changed(40) == ESP_FAIL);
    assert(first.level == 35 && second.level == 35);
    assert(solar_os_backlight_get_status(&status) == ESP_OK);
    assert(status.percent == 40 && status.effective_percent == 35 && status.last_error == ESP_FAIL);
    fail_save = true;
    assert(solar_os_backlight_set_brightness(45) == ESP_FAIL);
    assert(first.level == 35 && second.level == 35 && stored == 101);
    fail_save = false;
    assert(solar_os_backlight_display_changed(35) == ESP_OK);
    const unsigned sleep_saves = commits;
    assert(solar_os_backlight_set_suspended(true) == ESP_OK);
    assert(first.level == 0 && second.level == 0);
    assert(solar_os_backlight_display_changed(70) == ESP_OK);
    assert(first.level == 0 && stored == 101 && commits == sleep_saves);
    assert(solar_os_backlight_set_suspended(false) == ESP_OK && first.level == 70);
    assert(solar_os_backlight_set_brightness(15) == ESP_OK);
    assert(solar_os_backlight_display_changed(90) == ESP_OK && first.level == 15);
    second.fail_at = 60;
    assert(solar_os_backlight_set_brightness(60) == ESP_FAIL);
    assert(first.level == 15 && second.level == 15 && stored == 15);
    assert(solar_os_backlight_get_status(&status) == ESP_OK && status.percent == 15);
    assert(status.last_error == ESP_FAIL);
    fail_save = true;
    assert(solar_os_backlight_set_brightness(45) == ESP_FAIL);
    assert(first.level == 15 && second.level == 15 && stored == 15);
    fail_save = false;
    second.fail_at = 0;
    assert(solar_os_backlight_set_suspended(true) == ESP_FAIL);
    assert(first.level == 15 && second.level == 15);
    assert(solar_os_backlight_get_status(&status) == ESP_OK && !status.suspended);
    assert(solar_os_backlight_set_suspended(true) == ESP_OK);
    light_t late = {.fail_at = -1};
    assert(solar_os_backlight_register(set_light, &late) == ESP_OK && late.level == 0);
    assert(solar_os_backlight_set_brightness(22) == ESP_OK && first.level == 0);
    assert(solar_os_backlight_set_suspended(false) == ESP_OK && first.level == 22);
    assert(second.level == 22 && late.level == 22 && stored == 22);
    solar_os_backlight_unregister(&late);
    solar_os_backlight_unregister(&first);
    assert(first.level == 0);
    const unsigned calls = first.calls;
    assert(solar_os_backlight_set_brightness(0) == ESP_OK);
    assert(second.level == 0 && first.calls == calls);
    solar_os_backlight_unregister(&second);
    assert(solar_os_backlight_get_status(&status) == ESP_ERR_NOT_SUPPORTED);
    puts("Backlight settings, follow, suspend, rollback and provider lifetime passed");
    return 0;
}
