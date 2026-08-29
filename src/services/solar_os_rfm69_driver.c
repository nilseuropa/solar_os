#include "solar_os_rfm69.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
    {.key = "irq", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "irq"},
    {.key = "reset", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset"},
};

const solar_os_expansion_driver_t solar_os_rfm69_expansion_driver = {
    .name = "rfm69",
    .summary = "RFM69 packet radio",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_SPI,
    .probe_supported = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_rfm69_attach,
    .detach = solar_os_rfm69_detach,
};

const solar_os_expansion_driver_t solar_os_rfm69h_expansion_driver = {
    .name = "rfm69h",
    .summary = "RFM69 high-power radio",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_SPI,
    .probe_supported = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_rfm69h_attach,
    .detach = solar_os_rfm69_detach,
};
