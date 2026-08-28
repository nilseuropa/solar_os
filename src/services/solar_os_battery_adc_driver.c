#include "solar_os_battery_adc.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "adc", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_ADC, .role = "battery", .required = true},
    {.key = "divider", .value_hint = "1000..10000", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "divider", .required = true, .has_value_range = true, .min_value = 1000, .max_value = 10000},
};

const solar_os_expansion_driver_t solar_os_battery_adc_expansion_driver = {
    .name = "battery-adc",
    .summary = "ADC battery monitor",
    .required_capabilities = SOLAR_OS_BOARD_CAP_ADC,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_battery_adc_attach,
    .detach = solar_os_battery_adc_detach,
};
