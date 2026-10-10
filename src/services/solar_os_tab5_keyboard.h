#pragma once

#include "solar_os_expansion.h"
#include "solar_os_input_keymap.h"
#include "tab5_keyboard.h"

extern const solar_os_expansion_driver_t solar_os_tab5_keyboard_expansion_driver;
void solar_os_tab5_keyboard_map(solar_os_input_keymap_t *map);
esp_err_t solar_os_tab5_keyboard_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count);
esp_err_t solar_os_tab5_keyboard_detach(const char *name);
