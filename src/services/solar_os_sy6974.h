#pragma once

#include "solar_os_expansion.h"

extern const solar_os_expansion_driver_t solar_os_sy6974_expansion_driver;

esp_err_t solar_os_sy6974_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count);
esp_err_t solar_os_sy6974_detach(const char *name);
