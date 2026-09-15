#include "solar_os_bhi260ap.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x28", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true},
    {.key = "irq", .value_hint = "gpio", .role = "irq", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .required = true},
};

const solar_os_expansion_driver_t solar_os_bhi260ap_expansion_driver = {
    .name = "bhi260ap",
    .category = SOLAR_OS_EXPANSION_CATEGORY_SENSOR,
    .summary = "Bosch BHI260AP six-axis IMU",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C |
        SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .probe_supported = false,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_bhi260ap_attach,
    .detach = solar_os_bhi260ap_detach,
};
