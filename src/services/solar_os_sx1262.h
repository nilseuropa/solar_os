#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

esp_err_t solar_os_sx1262_attach(const char *name,
                                 const solar_os_expansion_binding_t *bindings,
                                 size_t binding_count);
esp_err_t solar_os_sx1262_detach(const char *name);
