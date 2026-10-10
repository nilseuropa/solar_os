#pragma once
#include "solar_os_expansion.h"
#include "solar_os_matrix_keyboard.h"
void solar_os_lilygo_pager_keyboard_map(solar_os_matrix_keyboard_map_t *map);
esp_err_t solar_os_lilygo_pager_keyboard_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count);
esp_err_t solar_os_lilygo_pager_keyboard_detach(const char *name);
