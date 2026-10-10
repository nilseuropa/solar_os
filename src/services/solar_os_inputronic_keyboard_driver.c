#include "solar_os_inputronic_keyboard.h"

static const int addresses[] = {SOLAR_OS_INPUTRONIC_KEYBOARD_ADDRESS};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x34", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true,
     .allowed_values = addresses, .allowed_value_count = 1},
    {.key = "reset", .value_hint = "gpio", .role = "reset", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "irq", .value_hint = "gpio", .role = "irq", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
};

const solar_os_expansion_driver_t solar_os_inputronic_keyboard_expansion_driver = {
    .name = "inputronic-keyboard",
    .category = SOLAR_OS_EXPANSION_CATEGORY_INPUT,
    .summary = "Soldered Inputronic 8x10 keyboard",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
    .probe_supported = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_inputronic_keyboard_attach,
    .detach = solar_os_inputronic_keyboard_detach,
};
