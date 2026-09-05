#include "solar_os_rotary_encoder.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "a", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "a", .required = true},
    {.key = "b", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "b", .required = true},
};

const solar_os_expansion_driver_t solar_os_rotary_encoder_expansion_driver = {
    .name = "rotary-encoder",
    .summary = "Quadrature rotary encoder (Up/Down per detent)",
    .required_capabilities = SOLAR_OS_BOARD_CAP_GPIO,
    .probe_supported = false,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_rotary_encoder_attach,
    .detach = solar_os_rotary_encoder_detach,
};
