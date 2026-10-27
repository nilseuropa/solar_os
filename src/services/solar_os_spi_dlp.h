#pragma once
#include "solar_os_expansion.h"
esp_err_t solar_os_spi_dlp_attach(const char *,
                                      const solar_os_expansion_binding_t *,
                                      size_t);
esp_err_t solar_os_spi_dlp_detach(const char *);
