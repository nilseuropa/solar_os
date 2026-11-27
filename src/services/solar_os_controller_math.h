#pragma once

#include <stdbool.h>

#include "solar_os_native_controllers_abi.h"

solar_os_controller_math_config_t solar_os_controller_math_defaults(
    solar_os_controller_kind_t kind);
bool solar_os_controller_math_valid(const solar_os_controller_math_config_t *config);
bool solar_os_controller_math_reset(const solar_os_controller_math_config_t *config,
    solar_os_controller_math_state_t *state, float output);
bool solar_os_controller_math_manual(const solar_os_controller_math_config_t *config,
    solar_os_controller_math_state_t *state, float output);
void solar_os_controller_math_auto(solar_os_controller_math_state_t *state);
/* Invalid input, arithmetic overflow, or dt outside (0,max_dt] returns false
 * without changing state. The caller must choose its fault output policy. */
bool solar_os_controller_math_step(const solar_os_controller_math_config_t *config,
    solar_os_controller_math_state_t *state, float setpoint, float measurement,
    float dt);
