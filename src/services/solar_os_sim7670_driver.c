#include "solar_os_sim7670.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "uart", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_UART_PORT, .required = true},
    {.key = "power", .value_hint = "gpio|controller:line", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "power"},
    {.key = "reset", .value_hint = "gpio|controller:line", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "reset"},
};

const solar_os_expansion_driver_t solar_os_sim7670_expansion_driver = {
    .name = "sim7670",
    .category = SOLAR_OS_EXPANSION_CATEGORY_RADIO,
    .summary = "SIM7670 LTE modem and GNSS receiver",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_UART,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_sim7670_attach,
    .detach = solar_os_sim7670_detach,
};
