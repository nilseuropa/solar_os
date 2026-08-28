#include "solar_os_ssd1683.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
    {.key = "dc", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc", .required = true},
    {.key = "reset", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .required = true},
    {.key = "busy", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "busy", .required = true},
};

const solar_os_expansion_driver_t solar_os_ssd1683_expansion_driver = {
    .name = "ssd1683",
    .summary = "400x300 e-paper",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_SPI | SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_ssd1683_attach,
    .detach = solar_os_ssd1683_detach,
};
