#include "solar_os_max17048.h"

#include "max17048.h"

static const int addresses[] = {MAX17048_I2C_ADDRESS};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x36", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
};

const solar_os_expansion_driver_t solar_os_max17048_expansion_driver = {
    .name = "max17048",
    .category = SOLAR_OS_EXPANSION_CATEGORY_POWER,
    .summary = "MAX17048 fuel gauge",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_max17048_attach,
    .detach = solar_os_max17048_detach,
};
