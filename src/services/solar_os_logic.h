#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "solar_os_native_logic_abi.h"

esp_err_t solar_os_logic_init(void);
esp_err_t solar_os_logic_default_config(solar_os_logic_config_t *config);
esp_err_t solar_os_logic_validate_config(const solar_os_logic_config_t *config);
esp_err_t solar_os_logic_capture(const solar_os_logic_config_t *config);
esp_err_t solar_os_logic_capture_with_options(const solar_os_logic_config_t *config,
    const solar_os_logic_capture_options_v1_t *options, uint32_t *generation);
esp_err_t solar_os_logic_copy_generation(uint32_t generation, size_t start,
    uint8_t *samples, size_t max_samples, size_t *copied);
esp_err_t solar_os_logic_get_status(solar_os_logic_status_t *status);
esp_err_t solar_os_logic_copy_samples(size_t start,
                                      uint8_t *samples,
                                      size_t max_samples,
                                      size_t *copied);
