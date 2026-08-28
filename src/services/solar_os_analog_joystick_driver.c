#include "solar_os_analog_joystick.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "x", .value_hint = "scalar-stream", .kind = SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM, .role = "x", .required = true},
    {.key = "y", .value_hint = "scalar-stream", .kind = SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM, .role = "y", .required = true},
    {.key = "min", .value_hint = "value", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "min", .required = true},
    {.key = "center", .value_hint = "value", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "center", .required = true},
    {.key = "max", .value_hint = "value", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "max", .required = true},
    {.key = "deadzone", .value_hint = "value", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "deadzone", .has_value_range = true, .min_value = 0, .max_value = 1000000},
};

const solar_os_expansion_driver_t solar_os_analog_joystick_expansion_driver = {
    .name = "analog-joystick",
    .summary = "two-axis joystick",
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_analog_joystick_attach,
    .detach = solar_os_analog_joystick_detach,
};
