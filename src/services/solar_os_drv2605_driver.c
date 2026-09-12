#include "solar_os_drv2605.h"

static const int addresses[] = {SOLAR_OS_DRV2605_ADDRESS};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x5A", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
};

const solar_os_expansion_driver_t solar_os_drv2605_expansion_driver = {
    .name = "drv2605",
    .category = SOLAR_OS_EXPANSION_CATEGORY_UTILITY,
    .summary = "DRV2605 haptic driver",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C,
    .probe_supported = false,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_drv2605_attach,
    .detach = solar_os_drv2605_detach,
};
