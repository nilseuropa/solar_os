#pragma once

#include "solar_os_expansion.h"

#define SOLAR_OS_INPUTRONIC_KEYBOARD_ADDRESS 0x34U

esp_err_t solar_os_inputronic_keyboard_attach(
    const char *name, const solar_os_expansion_binding_t *bindings,
    size_t binding_count);
esp_err_t solar_os_inputronic_keyboard_detach(const char *name);
