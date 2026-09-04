#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

#define SOLAR_OS_CL32_CORE_ADDRESS 0x08U

esp_err_t solar_os_cl32_core_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count);
esp_err_t solar_os_cl32_core_detach(const char *name);
