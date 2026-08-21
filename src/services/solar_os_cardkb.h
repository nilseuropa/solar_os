#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

#define SOLAR_OS_CARDKB_ADDRESS 0x5fU

esp_err_t solar_os_cardkb_attach(const char *name,
                                 const solar_os_expansion_binding_t *bindings,
                                 size_t binding_count);
esp_err_t solar_os_cardkb_detach(const char *name);
