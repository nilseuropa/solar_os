#include "solar_os_lsm6ds3trc.h"

#include "lsm6ds3trc.h"

static const int addresses[] = {
    LSM6DS3TRC_I2C_ADDRESS_LOW,
    LSM6DS3TRC_I2C_ADDRESS_HIGH,
};

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x6a|0x6b", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "irq", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "irq"},
    {.key = "rotation", .value_hint = "0..3", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "rotation", .has_value_range = true, .min_value = 0, .max_value = 3},
};

const solar_os_expansion_driver_t solar_os_lsm6ds3trc_expansion_driver = {
    .name = "lsm6ds3tr-c",
    .category = SOLAR_OS_EXPANSION_CATEGORY_SENSOR,
    .summary = "LSM6DS3TR-C six-axis IMU",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C,
    .probe_supported = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_lsm6ds3trc_attach,
    .detach = solar_os_lsm6ds3trc_detach,
};
