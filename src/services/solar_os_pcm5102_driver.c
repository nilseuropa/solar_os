#include "solar_os_pcm5102.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "bck", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "bck", .required = true},
    {.key = "din", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "din", .required = true},
    {.key = "rck", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "rck", .required = true},
};

const solar_os_expansion_driver_t solar_os_pcm5102_expansion_driver = {
    .name = "pcm5102",
    .summary = "I2S stereo audio",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2S,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_pcm5102_attach,
    .detach = solar_os_pcm5102_detach,
};
