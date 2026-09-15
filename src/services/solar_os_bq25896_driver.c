#include "solar_os_bq25896.h"

static const int addresses[] = {0x6B};

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x6b", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "charge_current", .value_hint = "0..3008mA/64", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "charge_current", .has_value_range = true, .min_value = 0, .max_value = 3008},
    {.key = "charge_voltage", .value_hint = "3840..4608mV/16", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "charge_voltage", .has_value_range = true, .min_value = 3840, .max_value = 4608},
};

const solar_os_expansion_driver_t solar_os_bq25896_expansion_driver = {
    .name = "bq25896",
    .category = SOLAR_OS_EXPANSION_CATEGORY_POWER,
    .summary = "TI BQ25896 battery charger",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
    .probe_supported = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_bq25896_attach,
    .detach = solar_os_bq25896_detach,
};
