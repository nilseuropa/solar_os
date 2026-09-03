#include "rtc_pcf85063.h"
#include "solar_os_pcf85063.h"

static const int addresses[] = {RTC_PCF85063_ADDRESS};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x51", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "irq", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "irq"},
};

const solar_os_expansion_driver_t solar_os_pcf85063_expansion_driver = {
    .name = "pcf85063",
    .summary = "I2C real-time clock",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C,
    .probe_supported = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_pcf85063_attach,
    .detach = solar_os_pcf85063_detach,
};
