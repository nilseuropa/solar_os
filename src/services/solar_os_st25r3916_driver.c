#include "solar_os_st25r3916.h"

static const int addresses[] = {0x50};

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS},
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS},
    {.key = "addr", .value_hint = "0x50", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "irq", .value_hint = "gpio", .role = "irq", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .required = true},
    {.key = "power", .value_hint = "gpio|controller:line", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE},
};
const solar_os_expansion_driver_t solar_os_st25r3916_expansion_driver = {
    .name = "st25r3916",
    .category = SOLAR_OS_EXPANSION_CATEGORY_UTILITY,
    .summary = "ST25R3916 NFC-A reader",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .probe_supported = false,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_st25r3916_attach,
    .detach = solar_os_st25r3916_detach,
};
