#pragma once

#include "solar_os_expansion.h"

esp_err_t solar_os_qemu_input_attach(const char *name,
                                     const solar_os_expansion_binding_t *bindings,
                                     size_t binding_count);
esp_err_t solar_os_qemu_input_detach(const char *name);
