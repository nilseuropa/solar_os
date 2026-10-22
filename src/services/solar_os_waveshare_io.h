#pragma once
#include "solar_os_expansion.h"

esp_err_t solar_os_waveshare_io_attach(const char *, const solar_os_expansion_binding_t *, size_t);
esp_err_t solar_os_waveshare_io_detach(const char *);
