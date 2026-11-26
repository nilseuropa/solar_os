#pragma once

#include "esp_err.h"
#include "solar_os_native_controls_abi.h"

typedef solar_os_native_control_pass_v1_t solar_os_control_pass_t;

/* Sampling can block on a scalar source. Binding application stays on the
 * caller task. Each kind of pass can run once at a time. */
esp_err_t solar_os_controls_sample(uint32_t now_ms,
                                  uint32_t window_ms,
                                  solar_os_control_pass_t *pass);
esp_err_t solar_os_controls_apply_bindings(solar_os_control_pass_t *pass);
