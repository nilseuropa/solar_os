#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

esp_err_t solar_os_pcm5102_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count);
esp_err_t solar_os_pcm5102_detach(const char *name);
extern const solar_os_expansion_driver_t solar_os_i2s_output_expansion_driver;
