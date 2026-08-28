#include "solar_os_neopixel.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "data", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "data", .required = true},
    {.key = "count", .value_hint = "1..256", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "count", .required = true, .has_value_range = true, .min_value = 1, .max_value = SOLAR_OS_NEOPIXEL_MAX_PIXELS},
};

const solar_os_expansion_driver_t solar_os_neopixel_expansion_driver = {
    .name = "neopixel",
    .summary = "WS2812 LED strip",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_neopixel_attach,
    .detach = solar_os_neopixel_detach,
};
