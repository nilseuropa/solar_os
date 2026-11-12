#include "solar_os_ip2315.h"

static const int addresses[] = {0x75};

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x75", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "gate", .value_hint = "gpio|controller:line", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "gate", .required = true},
    {.key = "watch_sda", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "watch_sda"},
    {.key = "watch_scl", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "watch_scl"},
};

const solar_os_expansion_driver_t solar_os_ip2315_expansion_driver = {
    .name = "ip2315",
    .category = SOLAR_OS_EXPANSION_CATEGORY_UTILITY,
    .summary = "IP2315 USB charger with gated I2C",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
    .probe_supported = true,
    .early = false,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_ip2315_attach,
    .detach = solar_os_ip2315_detach,
};
