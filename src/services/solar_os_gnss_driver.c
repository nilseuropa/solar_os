#include "solar_os_gnss.h"

#include "solar_os_board_caps.h"
#include "solar_os_expansion.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {
        .key        = "uart",
        .value_hint = "bus",
        .kind       = SOLAR_OS_EXPANSION_BINDING_UART_PORT,
        .required   = true,
    },
};

const solar_os_expansion_driver_t solar_os_gnss_expansion_driver = {
    .name                 = "uart-gnss",
    .summary              = "UART GNSS module",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_UART,
    .probe_supported      = false,
    .binding_specs        = binding_specs,
    .binding_spec_count   = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach               = solar_os_gnss_attach,
    .detach               = solar_os_gnss_detach,
};
