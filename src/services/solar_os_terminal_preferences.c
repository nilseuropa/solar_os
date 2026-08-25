#include "solar_os_terminal_preferences.h"

#include "nvs.h"
#include "solar_os_board.h"

#define TERM_NVS_NAMESPACE "terminal"
#define TERM_NVS_ORIENTATION_KEY "orientation"
#define TERM_NVS_FONT_KEY "font"
#define TERM_NVS_TEXT_SIZE_KEY "textsize"
#define TERM_NVS_PALETTE_KEY "palette"
#define TERM_NVS_STATUS_BAR_KEY "statusbar"
#define TERM_DEFAULT_FONT SOLAR_OS_TERMINAL_FONT_COMPACT
#define TERM_DEFAULT_TEXT_SIZE SOLAR_OS_TERMINAL_TEXT_SIZE_16

#ifndef SOLAR_OS_BOARD_DISPLAY_DEFAULT_ORIENTATION
#define SOLAR_OS_BOARD_DISPLAY_DEFAULT_ORIENTATION 0
#endif

static bool orientation_is_valid(uint16_t degrees)
{
    return degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270;
}

bool solar_os_terminal_profile_is_valid(const solar_os_terminal_profile_t *profile)
{
    return profile != NULL &&
        orientation_is_valid(profile->orientation_degrees) &&
        profile->font >= SOLAR_OS_TERMINAL_FONT_MONO &&
        profile->font <= SOLAR_OS_TERMINAL_FONT_COMPACT &&
        profile->text_size >= SOLAR_OS_TERMINAL_TEXT_SIZE_14 &&
        profile->text_size < SOLAR_OS_TERMINAL_TEXT_SIZE_COUNT;
}

void solar_os_terminal_profile_load_preferences(solar_os_terminal_profile_t *profile)
{
    if (profile == NULL) {
        return;
    }

    *profile = (solar_os_terminal_profile_t) {
        .orientation_degrees = SOLAR_OS_BOARD_DISPLAY_DEFAULT_ORIENTATION,
        .font = TERM_DEFAULT_FONT,
        .text_size = TERM_DEFAULT_TEXT_SIZE,
        .palette_inverted = false,
        .status_bar_visible = true,
    };

    nvs_handle_t nvs;
    if (nvs_open(TERM_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    uint16_t value = 0;
    if (nvs_get_u16(nvs, TERM_NVS_ORIENTATION_KEY, &value) == ESP_OK &&
        orientation_is_valid(value)) {
        profile->orientation_degrees = value;
    }
    if (nvs_get_u16(nvs, TERM_NVS_FONT_KEY, &value) == ESP_OK &&
        value <= SOLAR_OS_TERMINAL_FONT_COMPACT) {
        profile->font = (solar_os_terminal_font_t)value;
    }
    if (nvs_get_u16(nvs, TERM_NVS_TEXT_SIZE_KEY, &value) == ESP_OK &&
        value < SOLAR_OS_TERMINAL_TEXT_SIZE_COUNT) {
        profile->text_size = (solar_os_terminal_text_size_t)value;
    }
    if (nvs_get_u16(nvs, TERM_NVS_PALETTE_KEY, &value) == ESP_OK && value <= 1) {
        profile->palette_inverted = value != 0;
    }
    if (nvs_get_u16(nvs, TERM_NVS_STATUS_BAR_KEY, &value) == ESP_OK && value <= 1) {
        profile->status_bar_visible = value != 0;
    }

    nvs_close(nvs);
}

static esp_err_t save_u16(const char *key, uint16_t value)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(TERM_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u16(nvs, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

esp_err_t solar_os_terminal_profile_save_orientation(uint16_t degrees)
{
    return orientation_is_valid(degrees) ?
        save_u16(TERM_NVS_ORIENTATION_KEY, degrees) : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_terminal_profile_save_font(solar_os_terminal_font_t font)
{
    return font >= SOLAR_OS_TERMINAL_FONT_MONO &&
        font <= SOLAR_OS_TERMINAL_FONT_COMPACT ?
        save_u16(TERM_NVS_FONT_KEY, (uint16_t)font) : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_terminal_profile_save_text_size(
    solar_os_terminal_text_size_t text_size)
{
    return text_size >= SOLAR_OS_TERMINAL_TEXT_SIZE_14 &&
        text_size < SOLAR_OS_TERMINAL_TEXT_SIZE_COUNT ?
        save_u16(TERM_NVS_TEXT_SIZE_KEY, (uint16_t)text_size) : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_terminal_profile_save_palette(bool inverted)
{
    return save_u16(TERM_NVS_PALETTE_KEY, inverted ? 1 : 0);
}

esp_err_t solar_os_terminal_profile_save_status_bar(bool visible)
{
    return save_u16(TERM_NVS_STATUS_BAR_KEY, visible ? 1 : 0);
}
