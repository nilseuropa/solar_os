#pragma once

#include "solar_os_terminal.h"

bool solar_os_terminal_profile_is_valid(const solar_os_terminal_profile_t *profile);
void solar_os_terminal_profile_load_preferences(solar_os_terminal_profile_t *profile);
esp_err_t solar_os_terminal_profile_save_orientation(uint16_t degrees);
esp_err_t solar_os_terminal_profile_save_font(solar_os_terminal_font_t font);
esp_err_t solar_os_terminal_profile_save_text_size(
    solar_os_terminal_text_size_t text_size);
esp_err_t solar_os_terminal_profile_save_palette(bool inverted);
esp_err_t solar_os_terminal_profile_save_status_bar(bool visible);
