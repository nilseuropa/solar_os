#include "solar_os_pcm1808.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "mclk", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "mclk", .required = true},
    {.key = "bck", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "bck", .required = true},
    {.key = "ws", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "ws", .required = true},
    {.key = "dout", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dout", .required = true},
};

const solar_os_expansion_driver_t solar_os_pcm1808_expansion_driver = {
    .name = "pcm1808",
    .summary = "I2S stereo audio input",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2S,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_pcm1808_attach,
    .detach = solar_os_pcm1808_detach,
};
