#include "solar_os_ps2_keyboard_device.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "ps2", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_PS2_BUS, .required = true},
};

const solar_os_expansion_driver_t solar_os_ps2_keyboard_expansion_driver = {
    .name = "ps2-keyboard",
    .summary = "PS/2 keyboard",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_ps2_keyboard_attach,
    .detach = solar_os_ps2_keyboard_detach,
};
