#include "solar_os_es7210.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "i2s", .value_hint = "i2s0|i2s1", .kind = SOLAR_OS_EXPANSION_BINDING_I2S_PORT, .required = true},
    {.key = "mclk", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "mclk", .required = true},
    {.key = "bck", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "bck", .required = true},
    {.key = "ws", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "ws", .required = true},
    {.key = "din", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "din", .required = true},
};

const solar_os_expansion_driver_t solar_os_es7210_expansion_driver = {
    .name = "es7210",
    .category = SOLAR_OS_EXPANSION_CATEGORY_AUDIO,
    .summary = "ES7210 I2S microphone array",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C |
                             SOLAR_OS_BOARD_CAP_EXPANSION_I2S,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_es7210_attach,
    .detach = solar_os_es7210_detach,
};
