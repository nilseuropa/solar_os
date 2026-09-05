#include "solar_os_tlora_pager_core.h"

static const int addresses[] = {SOLAR_OS_TLORA_PAGER_CORE_ADDRESS};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x20", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
};

const solar_os_expansion_driver_t solar_os_tlora_pager_core_expansion_driver = {
    .name = "tlora-pager-core",
    .summary = "T-LoRa-Pager XL9555 power sequencer",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C,
    .probe_supported = false,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_tlora_pager_core_attach,
    .detach = solar_os_tlora_pager_core_detach,
};
