#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nvs.h"
#include "solar_os_terminal_preferences.h"

typedef struct {
    const char *key;
    bool present;
    uint16_t value;
} stored_value_t;

static stored_value_t stored_values[] = {
    {"orientation", false, 0},
    {"font", false, 0},
    {"textsize", false, 0},
    {"palette", false, 0},
    {"statusbar", false, 0},
};

static unsigned commit_count;

static stored_value_t *find_value(const char *key)
{
    for (size_t i = 0; i < sizeof(stored_values) / sizeof(stored_values[0]); i++) {
        if (strcmp(stored_values[i].key, key) == 0) {
            return &stored_values[i];
        }
    }
    return NULL;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "terminal") == 0);
    (void)mode;
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value)
{
    assert(handle == 1);
    stored_value_t *stored = find_value(key);
    if (stored == NULL || !stored->present) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *value = stored->value;
    return ESP_OK;
}

esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    assert(handle == 1);
    stored_value_t *stored = find_value(key);
    assert(stored != NULL);
    stored->present = true;
    stored->value = value;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    commit_count++;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1);
}

int main(void)
{
    solar_os_terminal_profile_t profile;
    solar_os_terminal_profile_load_preferences(&profile);
    assert(profile.orientation_degrees == 0);
    assert(profile.font == SOLAR_OS_TERMINAL_FONT_COMPACT);
    assert(profile.text_size == SOLAR_OS_TERMINAL_TEXT_SIZE_16);
    assert(!profile.palette_inverted);
    assert(profile.status_bar_visible);
    assert(solar_os_terminal_profile_is_valid(&profile));

    assert(solar_os_terminal_profile_save_orientation(90) == ESP_OK);
    assert(solar_os_terminal_profile_save_font(SOLAR_OS_TERMINAL_FONT_MONO) == ESP_OK);
    assert(solar_os_terminal_profile_save_text_size(
               SOLAR_OS_TERMINAL_TEXT_SIZE_10) == ESP_OK);
    assert(solar_os_terminal_profile_save_palette(true) == ESP_OK);
    assert(solar_os_terminal_profile_save_status_bar(false) == ESP_OK);
    assert(commit_count == 5);

    solar_os_terminal_profile_load_preferences(&profile);
    assert(profile.orientation_degrees == 90);
    assert(profile.font == SOLAR_OS_TERMINAL_FONT_MONO);
    assert(profile.text_size == SOLAR_OS_TERMINAL_TEXT_SIZE_10);
    assert(profile.palette_inverted);
    assert(!profile.status_bar_visible);

    find_value("orientation")->value = 45;
    find_value("font")->value = 99;
    find_value("textsize")->value = 99;
    find_value("palette")->value = 2;
    find_value("statusbar")->value = 2;
    solar_os_terminal_profile_load_preferences(&profile);
    assert(profile.orientation_degrees == 0);
    assert(profile.font == SOLAR_OS_TERMINAL_FONT_COMPACT);
    assert(profile.text_size == SOLAR_OS_TERMINAL_TEXT_SIZE_16);
    assert(!profile.palette_inverted);
    assert(profile.status_bar_visible);

    assert(solar_os_terminal_profile_save_orientation(45) == ESP_ERR_INVALID_ARG);
    assert(solar_os_terminal_profile_save_font((solar_os_terminal_font_t)99) ==
           ESP_ERR_INVALID_ARG);
    assert(solar_os_terminal_profile_save_text_size(
               SOLAR_OS_TERMINAL_TEXT_SIZE_COUNT) == ESP_ERR_INVALID_ARG);
    return 0;
}
