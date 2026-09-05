#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

/* Fixed TCA8418 I2C address (ADDR strap tied low on every known module,
 * including the T-LoRa-Pager). */
#define SOLAR_OS_TCA8418_ADDRESS 0x34

esp_err_t solar_os_tca8418_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count);
esp_err_t solar_os_tca8418_detach(const char *name);
