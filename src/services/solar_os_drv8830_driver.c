#include "solar_os_drv8830.h"

static const int addresses[] = {0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68};
static const solar_os_expansion_binding_spec_t bindings[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x60..0x68", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true,
        .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "int", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "int"},
    {.key = "max_mv", .value_hint = "5060", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER,
        .role = "max_mv", .has_value_range = true, .min_value = 482, .max_value = 5060},
};

const solar_os_expansion_driver_t solar_os_drv8830_expansion_driver = {
    .name = "drv8830", .category = SOLAR_OS_EXPANSION_CATEGORY_UTILITY,
    .summary = "TI DRV8830 brushed DC motor driver",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C, .probe_supported = true,
    .binding_specs = bindings, .binding_spec_count = sizeof(bindings) / sizeof(bindings[0]),
    .attach = solar_os_drv8830_attach, .detach = solar_os_drv8830_detach,
};
