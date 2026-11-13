#include "solar_os_sx1262.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
    {.key = "busy", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "busy", .required = true},
    {.key = "reset", .value_hint = "gpio|controller:line", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "reset"},
    {.key = "irq", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "irq"},
    {.key = "power", .value_hint = "gpio|controller:line", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "power"},
    {.key = "ant", .value_hint = "gpio|controller:line", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "ant"},
    {.key = "frequency", .value_hint = "150000000..960000000", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "frequency", .has_value_range = true, .min_value = 150000000, .max_value = 960000000},
    {.key = "regulator", .value_hint = "0=dcdc,1=ldo", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "regulator", .has_value_range = true, .min_value = 0, .max_value = 1},
};

const solar_os_expansion_driver_t solar_os_sx1262_expansion_driver = {
    .name = "sx1262",
    .category = SOLAR_OS_EXPANSION_CATEGORY_RADIO,
    .summary = "Semtech SX1262 LoRa/(G)FSK radio",
    .required_capabilities = SOLAR_OS_BOARD_CAP_SPI,
    .probe_supported = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_sx1262_attach,
    .detach = solar_os_sx1262_detach,
};
