#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "solar_os_terminal_preferences.h"

#ifndef SOLAR_OS_BOARD_DISPLAY_DEFAULT_SCALE
#define SOLAR_OS_BOARD_DISPLAY_DEFAULT_SCALE 1
#endif

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
    {"cursor", false, 0},
};

static unsigned commit_count;
static uint8_t display_scales[2];
static esp_err_t scale_write_error;
static uint16_t display_orientations[2];
static bool orientation_present[2];
static esp_err_t orientation_write_error;

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
    assert(strcmp(name, "terminal") == 0 || strcmp(name, "display_scale") == 0 || !strcmp(name,"display_orient"));
    (void)mode;
    *handle = strcmp(name, "terminal") == 0 ? 1 : !strcmp(name,"display_scale") ? 2 : 3;
    return ESP_OK;
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value)
{
    if (handle == 3) {
        const unsigned index = !strcmp(key,"lcd0");
        if (!orientation_present[index]) return ESP_ERR_NVS_NOT_FOUND;
        *value=display_orientations[index];return ESP_OK;
    }
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
    if (orientation_write_error != ESP_OK) return orientation_write_error;
    if (handle == 3) {
        const unsigned index = !strcmp(key,"lcd0");
        orientation_present[index]=true;display_orientations[index]=value;return ESP_OK;
    }
    assert(handle == 1);
    stored_value_t *stored = find_value(key);
    assert(stored != NULL);
    stored->present = true;
    stored->value = value;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle >= 1 && handle <= 3);
    commit_count++;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle >= 1 && handle <= 3);
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    assert(handle == 2 && (!strcmp(key,"display0") || !strcmp(key,"oled0")));
    uint8_t scale = display_scales[!strcmp(key,"oled0")];
    if (scale == 0) return ESP_ERR_NVS_NOT_FOUND;
    *value=scale; return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle == 2);
    if (scale_write_error != ESP_OK) return scale_write_error;
    display_scales[!strcmp(key,"oled0")]=value; return ESP_OK;
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        const unsigned saved = (unsigned)strtoul(argv[1], NULL, 10);
        find_value("cursor")->present = true;
        find_value("cursor")->value = saved;
        solar_os_terminal_cursor_shape_t shape;
        bool blink;
        solar_os_terminal_get_cursor_preference(&shape, &blink);
        assert(shape == (saved <= 3 ? (solar_os_terminal_cursor_shape_t)(saved >> 1) :
                                       SOLAR_OS_TERMINAL_CURSOR_BLOCK));
        assert(blink == (saved > 3 || !(saved & 1U)));
        return 0;
    }
    solar_os_terminal_profile_t profile;
    solar_os_terminal_profile_load_preferences(&profile);
    assert(profile.orientation_degrees == 0);
    assert(profile.font == SOLAR_OS_TERMINAL_FONT_COMPACT);
    assert(profile.text_size == SOLAR_OS_TERMINAL_TEXT_SIZE_16);
    assert(!profile.palette_inverted);
    assert(profile.status_bar_visible);
    assert(profile.scale == 1);
    assert(solar_os_terminal_profile_is_valid(&profile));

    solar_os_terminal_cursor_shape_t shape;
    bool blink;
    solar_os_terminal_get_cursor_preference(&shape, &blink);
    assert(shape == SOLAR_OS_TERMINAL_CURSOR_BLOCK && blink);
    assert(solar_os_terminal_parse_cursor_shape("block", &shape));
    assert(shape == SOLAR_OS_TERMINAL_CURSOR_BLOCK);
    assert(solar_os_terminal_parse_cursor_shape("underscore", &shape));
    assert(shape == SOLAR_OS_TERMINAL_CURSOR_UNDERSCORE);
    assert(!solar_os_terminal_parse_cursor_shape("bar", &shape));
    assert(!solar_os_terminal_parse_cursor_shape(NULL, &shape));
    assert(!solar_os_terminal_parse_cursor_shape("block", NULL));

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

    assert(solar_os_terminal_profile_save_text_size(
               SOLAR_OS_TERMINAL_TEXT_SIZE_8) == ESP_OK);
    solar_os_terminal_profile_load_preferences(&profile);
    assert(profile.text_size == SOLAR_OS_TERMINAL_TEXT_SIZE_8);
    assert(solar_os_terminal_profile_is_valid(&profile));

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
    assert(solar_os_terminal_profile_load_scale("display0")==SOLAR_OS_BOARD_DISPLAY_DEFAULT_SCALE);
    assert(solar_os_terminal_profile_save_scale("display0",2)==ESP_OK);
    assert(solar_os_terminal_profile_load_scale("display0")==2);
    assert(solar_os_terminal_profile_load_scale("oled0")==1);
    assert(solar_os_terminal_profile_save_scale("oled0",3)==ESP_OK);
    assert(solar_os_terminal_profile_load_scale("display0")==2);
    assert(solar_os_terminal_profile_load_scale("oled0")==3);
    unsigned commits=commit_count;
    assert(solar_os_terminal_profile_save_scale("oled0",3)==ESP_OK && commit_count==commits);
    scale_write_error=ESP_FAIL;
    assert(solar_os_terminal_profile_save_scale("display0",4)==ESP_FAIL);
    assert(solar_os_terminal_profile_load_scale("display0")==2);
    assert(solar_os_terminal_profile_save_scale("display0",0)==ESP_ERR_INVALID_ARG);
    assert(solar_os_terminal_profile_save_scale("display0",5)==ESP_ERR_INVALID_ARG);
    assert(solar_os_terminal_profile_save_scale("",2)==ESP_ERR_INVALID_ARG);
    assert(solar_os_terminal_profile_save_scale("abcdefghijklmnop",2)==ESP_ERR_INVALID_ARG);
    display_scales[0]=99;
    assert(solar_os_terminal_profile_load_scale("display0")==SOLAR_OS_BOARD_DISPLAY_DEFAULT_SCALE);
    scale_write_error=ESP_OK;
    assert(solar_os_terminal_profile_save_scale("display0",1)==ESP_OK);
    assert(solar_os_terminal_profile_load_scale("display0")==1); /* saved user override */
    assert(solar_os_terminal_profile_load_display_orientation("display0",0)==0);
    assert(solar_os_terminal_profile_save_display_orientation("display0",270)==ESP_OK);
    assert(find_value("orientation")->value==270);
    solar_os_terminal_profile_load_preferences(&profile);
    assert(profile.orientation_degrees==270);
    assert(solar_os_terminal_profile_load_display_orientation("oled0",0)==0);
    assert(solar_os_terminal_profile_save_display_orientation("oled0",90)==ESP_OK);
    assert(solar_os_terminal_profile_save_display_orientation("lcd0",180)==ESP_OK);
    assert(solar_os_terminal_profile_load_display_orientation("oled0",0)==90);
    assert(solar_os_terminal_profile_load_display_orientation("lcd0",0)==180);
    assert(solar_os_terminal_profile_load_display_orientation("display0",0)==270);
    commits=commit_count;
    assert(solar_os_terminal_profile_save_display_orientation("oled0",90)==ESP_OK && commit_count==commits);
    orientation_write_error=ESP_FAIL;
    assert(solar_os_terminal_profile_save_display_orientation("oled0",270)==ESP_FAIL);
    assert(solar_os_terminal_profile_load_display_orientation("oled0",0)==90);
    assert(solar_os_terminal_profile_save_display_orientation("oled0",45)==ESP_ERR_INVALID_ARG);
    assert(solar_os_terminal_profile_save_display_orientation("",90)==ESP_ERR_INVALID_ARG);
    assert(solar_os_terminal_profile_save_display_orientation("abcdefghijklmnop",90)==ESP_ERR_INVALID_ARG);
    display_orientations[0]=45;
    assert(solar_os_terminal_profile_load_display_orientation("oled0",180)==180);
    orientation_write_error = ESP_OK;
    for (unsigned value = 0; value < 4; value++) {
        shape = (solar_os_terminal_cursor_shape_t)(value >> 1U);
        blink = !(value & 1U);
        assert(solar_os_terminal_set_cursor_preference(shape, blink) == ESP_OK);
        assert(find_value("cursor")->value == value);
        solar_os_terminal_cursor_shape_t actual;
        bool actual_blink;
        solar_os_terminal_get_cursor_preference(&actual, &actual_blink);
        assert(actual == shape && actual_blink == blink);
    }
    assert(solar_os_terminal_set_cursor_preference((solar_os_terminal_cursor_shape_t)2, true) ==
           ESP_ERR_INVALID_ARG);
    orientation_write_error = ESP_FAIL;
    assert(solar_os_terminal_set_cursor_preference(SOLAR_OS_TERMINAL_CURSOR_BLOCK, true) == ESP_FAIL);
    solar_os_terminal_get_cursor_preference(&shape, &blink);
    assert(shape == SOLAR_OS_TERMINAL_CURSOR_UNDERSCORE && !blink);
    return 0;
}
