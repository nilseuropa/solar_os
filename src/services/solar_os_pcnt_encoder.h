#pragma once

#include "solar_os_expansion.h"

esp_err_t solar_os_pcnt_encoder_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count);
esp_err_t solar_os_pcnt_encoder_detach(const char *name);
esp_err_t solar_os_pcnt_encoder_validate(const solar_os_expansion_binding_t *bindings,
    size_t count, solar_os_expansion_binding_validation_t *validation);
