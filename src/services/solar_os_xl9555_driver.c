#include "solar_os_xl9555.h"

static const int addresses[] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27};

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x20..0x27", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "output", .value_hint = "0..65535", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "output", .has_value_range = true, .min_value = 0, .max_value = UINT16_MAX},
    {.key = "direction", .value_hint = "0..65535", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "direction", .has_value_range = true, .min_value = 0, .max_value = UINT16_MAX},
};

const solar_os_expansion_driver_t solar_os_xl9555_expansion_driver = {
    .name = "xl9555",
    .category = SOLAR_OS_EXPANSION_CATEGORY_UTILITY,
    .summary = "XL9555 16-line I2C GPIO controller",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
    .probe_supported = true,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_xl9555_attach,
    .detach = solar_os_xl9555_detach,
};
