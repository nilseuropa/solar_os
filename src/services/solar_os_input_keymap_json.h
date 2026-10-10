#pragma once
#include "solar_os_input_keymap_mapper.h"
esp_err_t solar_os_input_keymap_parse(const char *json,
    const solar_os_input_keymap_t *base, solar_os_input_keymap_t *out);
