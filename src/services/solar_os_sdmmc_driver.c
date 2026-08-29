#include "solar_os_sdmmc.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "clk", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "clk", .required = true},
    {.key = "cmd", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "cmd", .required = true},
    {.key = "d0", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "d0", .required = true},
    {.key = "d1", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "d1"},
    {.key = "d2", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "d2"},
    {.key = "d3", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "d3"},
};

const solar_os_expansion_driver_t solar_os_sdmmc_expansion_driver = {
    .name = "sdmmc",
    .summary = "SDMMC card",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .probe_supported = true,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_sdmmc_attach,
    .detach = solar_os_sdmmc_detach,
};
