#pragma once
#include "solar_os_expansion.h"

esp_err_t solar_os_inmp441_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t count);
esp_err_t solar_os_inmp441_detach(const char *name);
esp_err_t solar_os_ics43432_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t count);
