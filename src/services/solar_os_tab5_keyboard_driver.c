#include "solar_os_tab5_keyboard.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x6d", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS,
     .required = true, .has_value_range = true, .min_value = 0x08, .max_value = 0x77},
    {.key = "irq", .value_hint = "gpio", .role = "irq", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
};

const solar_os_expansion_driver_t solar_os_tab5_keyboard_expansion_driver = {
    .name = "tab5-keyboard",
    .category = SOLAR_OS_EXPANSION_CATEGORY_INPUT,
    .summary = "M5Stack Tab5 70-key keyboard",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
    .probe_supported = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_tab5_keyboard_attach,
    .detach = solar_os_tab5_keyboard_detach,
};
