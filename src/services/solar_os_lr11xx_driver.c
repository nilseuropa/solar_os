#include "solar_os_lr11xx.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
    {.key = "busy", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "busy", .required = true},
    {.key = "reset", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset"},
    {.key = "irq", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "irq"},
    /* Every board ships a different SKU, so the band is the board's to state.
     * The 2.4 GHz path of the LR1120 and LR1121 is not driven. */
    {.key = "freq", .value_hint = "150..960 MHz", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER,
     .role = "freq", .has_value_range = true, .min_value = 150, .max_value = 960},
    /* Millivolts on the TCXO supply DIO, or 0 when the board clocks the part
     * from a plain crystal. There is no safe default: telling a crystal board
     * to power a TCXO stalls the part's clock start-up. */
    {.key = "tcxo", .value_hint = "0|1600..3300 mV", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER,
     .role = "tcxo", .has_value_range = true, .min_value = 0, .max_value = 3300},
};

const solar_os_expansion_driver_t solar_os_lr11xx_expansion_driver = {
    .name = "lr11xx",
    .category = SOLAR_OS_EXPANSION_CATEGORY_RADIO,
    .summary = "Semtech LR11xx LoRa radio",
    .required_capabilities = SOLAR_OS_BOARD_CAP_SPI,
    .probe_supported = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_lr11xx_attach,
    .detach = solar_os_lr11xx_detach,
};
