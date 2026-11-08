#pragma once

#include "solar_os_expansion.h"

extern const solar_os_expansion_driver_t solar_os_gpio_matrix_expansion_driver;
esp_err_t solar_os_gpio_matrix_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count);
esp_err_t solar_os_gpio_matrix_detach(const char *name);
