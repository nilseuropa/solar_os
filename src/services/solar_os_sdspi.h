#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

#define SOLAR_OS_SDSPI_DIAGNOSTICS_MAX 640

esp_err_t solar_os_sdspi_attach(const char *name,
                                const solar_os_expansion_binding_t *bindings,
                                size_t binding_count);
esp_err_t solar_os_sdspi_detach(const char *name);
size_t solar_os_sdspi_format_last_diagnostics(char *buffer, size_t len);
