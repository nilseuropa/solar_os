#include "solar_os_mgc3130.h"

static const int addresses[] = {0x42, 0x43};
static const int airwheel_values[] = {0, 1};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x42|0x43", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "transfer", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "transfer", .required = true},
    {.key = "reset", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .required = true},
    {.key = "rotation", .value_hint = "0..3", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "rotation", .required = true, .has_value_range = true, .min_value = 0, .max_value = 3},
    {.key = "airwheel", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "airwheel", .allowed_values = airwheel_values, .allowed_value_count = sizeof(airwheel_values) / sizeof(airwheel_values[0])},
};

const solar_os_expansion_driver_t solar_os_mgc3130_expansion_driver = {
    .name = "mgc3130",
    .category = SOLAR_OS_EXPANSION_CATEGORY_INPUT,
    .summary = "MGC3130 3D gesture and touch sensor",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C |
        SOLAR_OS_BOARD_CAP_EXPANSION_GPIO | SOLAR_OS_BOARD_CAP_GFX,
    .probe_supported = false,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_mgc3130_attach,
    .detach = solar_os_mgc3130_detach,
};
