#pragma once
#include "solar_os_expansion.h"

esp_err_t solar_os_es7243_attach(const char *, const solar_os_expansion_binding_t *, size_t);
esp_err_t solar_os_es7243e_attach(const char *, const solar_os_expansion_binding_t *, size_t);
esp_err_t solar_os_es7243_detach(const char *);
