#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "solar_os_expansion.h"
#include "solar_os_matrix_keyboard.h"

/* TI TCA8418 has a fixed 7-bit I2C address. */
#define SOLAR_OS_TCA8418_ADDRESS 0x34

esp_err_t solar_os_tca8418_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count);
esp_err_t solar_os_tca8418_detach(const char *name);

/* Branded drivers supply their wiring map; NULL selects the generic map. */
esp_err_t solar_os_tca8418_attach_profile(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t binding_count,
    const solar_os_matrix_keyboard_map_t *profile);
esp_err_t solar_os_tca8418_get_keymap(const char *name, bool defaults,
    solar_os_matrix_keyboard_map_t *map);
esp_err_t solar_os_tca8418_set_keymap(const char *name,
    const solar_os_matrix_keyboard_map_t *map);
esp_err_t solar_os_tca8418_reset_keymap(const char *name);
esp_err_t solar_os_tca8418_load_keymap(const char *name, const char *path);
