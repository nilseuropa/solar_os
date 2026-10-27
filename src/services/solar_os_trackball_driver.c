#include "solar_os_trackball.h"

static const solar_os_expansion_binding_spec_t bindings[] = {
    {.key = "up",
     .role = "up",
     .value_hint = "gpio",
     .kind = SOLAR_OS_EXPANSION_BINDING_GPIO,
     .required = true},
    {.key = "down",
     .role = "down",
     .value_hint = "gpio",
     .kind = SOLAR_OS_EXPANSION_BINDING_GPIO,
     .required = true},
    {.key = "left",
     .role = "left",
     .value_hint = "gpio",
     .kind = SOLAR_OS_EXPANSION_BINDING_GPIO,
     .required = true},
    {.key = "right",
     .role = "right",
     .value_hint = "gpio",
     .kind = SOLAR_OS_EXPANSION_BINDING_GPIO,
     .required = true},
    {.key = "pull",
     .role = "pull",
     .value_hint = "0=none|1=up|2=down",
     .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER},
};

const solar_os_expansion_driver_t solar_os_trackball_expansion_driver = {
    .name = "trackball",
    .category = SOLAR_OS_EXPANSION_CATEGORY_INPUT,
    .summary = "four-channel interrupt trackball",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .binding_specs = bindings,
    .binding_spec_count = sizeof(bindings) / sizeof(bindings[0]),
    .attach = solar_os_trackball_attach,
    .detach = solar_os_trackball_detach,
};
