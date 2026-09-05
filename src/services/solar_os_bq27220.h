#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

/* Default 7-bit I2C address for the TI BQ27220 fuel gauge. */
#define SOLAR_OS_BQ27220_ADDRESS 0x55

esp_err_t solar_os_bq27220_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count);
esp_err_t solar_os_bq27220_detach(const char *name);
