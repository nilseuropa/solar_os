#include "solar_os_ublox_mia_m10q.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "uart", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_UART_PORT, .required = true},
    {.key = "power", .value_hint = "gpio|controller:line", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE},
};
const solar_os_expansion_driver_t solar_os_ublox_mia_m10q_expansion_driver = {
    .name = "ublox-mia-m10q",
    .category = SOLAR_OS_EXPANSION_CATEGORY_SENSOR,
    .summary = "u-blox MIA-M10Q GNSS receiver",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_UART,
    .probe_supported = false,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_ublox_mia_m10q_attach,
    .detach = solar_os_ublox_mia_m10q_detach,
};
