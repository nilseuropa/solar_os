#include "solar_os_ssd2677.h"

static const int power_addresses[] = {0x34};

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
    {.key = "dc", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc", .required = true},
    {.key = "reset", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .required = true},
    {.key = "busy", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "busy", .required = true},
    {.key = "power", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "power"},
    {.key = "power_i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .role = "power"},
    {.key = "power_addr", .value_hint = "0x34", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "power_addr", .allowed_values = power_addresses, .allowed_value_count = sizeof(power_addresses) / sizeof(power_addresses[0])},
    {.key = "clock", .value_hint = "khz", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "clock", .has_value_range = true, .min_value = 100, .max_value = 20000},
    {.key = "rotation", .value_hint = "0..3", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "rotation", .has_value_range = true, .min_value = 0, .max_value = 3},
    {.key = "panel", .value_hint = "0|1=Sticky 3D97 V2", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "panel", .has_value_range = true, .min_value = 0, .max_value = 1},
};

const solar_os_expansion_driver_t solar_os_ssd2677_expansion_driver = {
    .name = "ssd2677",
    .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .summary = "SSD2677 800x480 e-paper",
    .required_capabilities = SOLAR_OS_BOARD_CAP_GFX |
                             SOLAR_OS_BOARD_CAP_EXPANSION_SPI |
                             SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_ssd2677_attach,
    .detach = solar_os_ssd2677_detach,
};
