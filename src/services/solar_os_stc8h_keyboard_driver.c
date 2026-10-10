#include "solar_os_stc8h_keyboard.h"

static const int addresses[] = {SOLAR_OS_STC8H_KEYBOARD_ADDRESS,
                                SOLAR_OS_STC8H_KEYBOARD_ALT_ADDRESS};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x6c", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "alt_addr", .value_hint = "0x6d", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .role = "alt_addr", .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "backlight", .value_hint = "gpio", .role = "backlight", .kind = SOLAR_OS_EXPANSION_BINDING_PWM, .required = false},
};

const solar_os_expansion_driver_t solar_os_stc8h_keyboard_expansion_driver = {
    .name = "stc8h-keyboard",
    .category = SOLAR_OS_EXPANSION_CATEGORY_INPUT,
    .summary = "Elecrow STC8H companion-MCU keypad",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C | SOLAR_OS_BOARD_CAP_PWM,
    .probe_supported = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_stc8h_keyboard_attach,
    .detach = solar_os_stc8h_keyboard_detach,
};
