#include "solar_os_ssd1306.h"

static const int addresses[] = {0x3c, 0x3d};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x3c|0x3d", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
};

const solar_os_expansion_driver_t solar_os_ssd1306_expansion_driver = {
    .name = "ssd1306",
    .summary = "128x64 I2C OLED",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
    .probe_supported = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_ssd1306_attach,
    .detach = solar_os_ssd1306_detach,
};

const solar_os_expansion_driver_t solar_os_sh1106_expansion_driver = {
    .name = "sh1106",
    .summary = "128x64 I2C OLED",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
    .probe_supported = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_sh1106_attach,
    .detach = solar_os_ssd1306_detach,
};
