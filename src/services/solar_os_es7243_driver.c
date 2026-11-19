#include "solar_os_es7243.h"

static const solar_os_expansion_binding_spec_t bindings[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "7-bit address", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .has_value_range = true, .min_value = 0x08, .max_value = 0x77},
    {.key = "i2s", .value_hint = "i2s0|i2s1", .kind = SOLAR_OS_EXPANSION_BINDING_I2S_PORT, .required = true},
    {.key = "mclk", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "mclk", .required = true},
    {.key = "bck", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "bck", .required = true},
    {.key = "ws", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "ws", .required = true},
    {.key = "din", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "din", .required = true},
};

const solar_os_expansion_driver_t solar_os_es7243_expansion_driver = {
    .name = "es7243", .category = SOLAR_OS_EXPANSION_CATEGORY_AUDIO,
    .summary = "ES7243 stereo I2S microphone ADC",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C | SOLAR_OS_BOARD_CAP_EXPANSION_I2S,
    .binding_specs = bindings, .binding_spec_count = sizeof(bindings) / sizeof(bindings[0]),
    .attach = solar_os_es7243_attach, .detach = solar_os_es7243_detach,
};
const solar_os_expansion_driver_t solar_os_es7243e_expansion_driver = {
    .name = "es7243e", .category = SOLAR_OS_EXPANSION_CATEGORY_AUDIO,
    .summary = "ES7243E stereo I2S microphone ADC",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C | SOLAR_OS_BOARD_CAP_EXPANSION_I2S,
    .binding_specs = bindings, .binding_spec_count = sizeof(bindings) / sizeof(bindings[0]),
    .attach = solar_os_es7243e_attach, .detach = solar_os_es7243_detach,
};
