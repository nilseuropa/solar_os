#include "solar_os_sdspi.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
    {.key = "power", .value_hint = "gpio|controller:line", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE},
};

const solar_os_expansion_driver_t solar_os_sdspi_expansion_driver = {
    .name = "sdspi",
    .category = SOLAR_OS_EXPANSION_CATEGORY_STORAGE,
    .summary = "SPI microSD",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_SPI,
    .probe_supported = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_sdspi_attach,
    .detach = solar_os_sdspi_detach,
};
