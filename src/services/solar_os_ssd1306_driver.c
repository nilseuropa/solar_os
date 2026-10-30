#include "solar_os_ssd1306.h"

static const int addresses[] = {0x3c, 0x3d};
static const solar_os_expansion_binding_spec_t ssd1306_bindings[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x3c|0x3d", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "panel", .value_hint = "0..7", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "panel", .has_value_range = true, .min_value = 0, .max_value = 7},
    {.key = "rotation", .value_hint = "0..3", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "rotation", .has_value_range = true, .min_value = 0, .max_value = 3},
};

static const solar_os_expansion_binding_spec_t sh1106_bindings[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x3c|0x3d", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "panel", .value_hint = "0..3", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "panel", .has_value_range = true, .min_value = 0, .max_value = 3},
    {.key = "rotation", .value_hint = "0..3", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "rotation", .has_value_range = true, .min_value = 0, .max_value = 3},
};

const solar_os_expansion_driver_t solar_os_ssd1306_expansion_driver = {
    .name = "ssd1306",
    .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .summary = "SSD1306 I2C OLED",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
    .probe_supported = true,
    .binding_specs = ssd1306_bindings,
    .binding_spec_count = sizeof(ssd1306_bindings) / sizeof(ssd1306_bindings[0]),
    .attach = solar_os_ssd1306_attach,
    .detach = solar_os_ssd1306_detach,
};

const solar_os_expansion_driver_t solar_os_sh1106_expansion_driver = {
    .name = "sh1106",
    .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .summary = "SH1106 I2C OLED",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
    .probe_supported = true,
    .binding_specs = sh1106_bindings,
    .binding_spec_count = sizeof(sh1106_bindings) / sizeof(sh1106_bindings[0]),
    .attach = solar_os_sh1106_attach,
    .detach = solar_os_ssd1306_detach,
};
