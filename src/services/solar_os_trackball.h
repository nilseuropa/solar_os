#pragma once

#include "solar_os_expansion.h"

esp_err_t solar_os_trackball_attach(const char *name, const solar_os_expansion_binding_t *bindings,
                                    size_t count);
esp_err_t solar_os_trackball_detach(const char *name);
void solar_os_trackball_poll(void);
bool solar_os_trackball_active(void);
