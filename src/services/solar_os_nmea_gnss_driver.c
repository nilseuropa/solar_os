#include "solar_os_nmea_gnss.h"

static const int bool_values[] = {0, 1};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "uart", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_UART_PORT, .required = true},
    {.key = "power", .value_hint = "gpio|controller:line", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "power"},
    {.key = "active", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "active", .allowed_values = bool_values, .allowed_value_count = 2},
    {.key = "alt_baud", .value_hint = "4800..921600", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "alt_baud", .has_value_range = true, .min_value = 4800, .max_value = 921600},
};
const solar_os_expansion_driver_t solar_os_nmea_gnss_expansion_driver = {
    .name = "nmea",
    .category = SOLAR_OS_EXPANSION_CATEGORY_SENSOR,
    .summary = "NMEA 0183 GNSS receiver",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_UART,
    .probe_supported = false,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_nmea_gnss_attach,
    .detach = solar_os_nmea_gnss_detach,
};
