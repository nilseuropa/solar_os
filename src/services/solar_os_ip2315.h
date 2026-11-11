#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

typedef bool (*solar_os_ip2315_vin_present_fn)(void);
void solar_os_ip2315_set_vin_present_fn(solar_os_ip2315_vin_present_fn fn);


esp_err_t solar_os_ip2315_attach(const char *name,
                                 const solar_os_expansion_binding_t *bindings,
                                 size_t binding_count);
esp_err_t solar_os_ip2315_detach(const char *name);
