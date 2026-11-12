#include "solar_os_m5ioe1.h"

static const int addresses[] = {0x4F};

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x4f", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "output", .value_hint = "0..16383", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "output", .has_value_range = true, .min_value = 0, .max_value = 16383},
    {.key = "direction", .value_hint = "0..16383", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "direction", .has_value_range = true, .min_value = 0, .max_value = 16383},
};

const solar_os_expansion_driver_t solar_os_m5ioe1_expansion_driver = {
    .name = "m5ioe1",
    .category = SOLAR_OS_EXPANSION_CATEGORY_UTILITY,
    .summary = "M5IOE1 14-line I2C GPIO controller",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
    .probe_supported = true,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_m5ioe1_attach,
    .detach = solar_os_m5ioe1_detach,
};
