#include "solar_os_sy6974.h"

static const int addresses[] = {0x6B};

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "input_voltage", .value_hint = "3900..5400mV/100", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "input_voltage", .has_value_range = true, .min_value = 3900, .max_value = 5400},
    {.key = "adc", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_ADC},
    {.key = "power", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "power"},
    {.key = "divider", .value_hint = "1000..10000", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "divider", .has_value_range = true, .min_value = 1000, .max_value = 10000},
    {.key = "input_current", .value_hint = "100..3200mA/100", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "input_current", .has_value_range = true, .min_value = 100, .max_value = 3200},
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x6b", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "charge_current", .value_hint = "0..3000mA/60", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "charge_current", .has_value_range = true, .min_value = 0, .max_value = 3000},
    {.key = "charge_voltage", .value_hint = "3856..4624mV/32", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "charge_voltage", .has_value_range = true, .min_value = 3856, .max_value = 4624},
};

const solar_os_expansion_driver_t solar_os_sy6974_expansion_driver = {
    .name = "sy6974",
    .category = SOLAR_OS_EXPANSION_CATEGORY_POWER,
    .summary = "Silergy SY6974B battery charger",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
    .probe_supported = true,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_sy6974_attach,
    .detach = solar_os_sy6974_detach,
};
