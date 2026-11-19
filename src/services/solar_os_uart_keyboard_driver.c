#include "solar_os_uart_keyboard.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "uart", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_UART_PORT, .required = true},
};

const solar_os_expansion_driver_t solar_os_uart_keyboard_expansion_driver = {
    .name = "uart-keyboard",
    .category = SOLAR_OS_EXPANSION_CATEGORY_INPUT,
    .summary = "Terminal keyboard on a UART",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_UART,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_uart_keyboard_attach,
    .detach = solar_os_uart_keyboard_detach,
};
