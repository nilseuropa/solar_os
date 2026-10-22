#include "solar_os_waveshare_io.h"

static const int addresses[] = {0x24};
static const solar_os_expansion_binding_spec_t specs[] = {
    {.key="i2c", .value_hint="bus", .kind=SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required=true},
    {.key="addr", .value_hint="0x24", .kind=SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required=true, .allowed_values=addresses, .allowed_value_count=1},
    {.key="output", .value_hint="0..255", .kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role="output", .has_value_range=true, .min_value=0, .max_value=255},
    {.key="direction", .value_hint="0..255", .kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role="direction", .has_value_range=true, .min_value=0, .max_value=255},
    {.key="battery", .value_hint="0|1", .kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role="battery", .has_value_range=true, .min_value=0, .max_value=1},
    {.key="backlight", .value_hint="0|1", .kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role="backlight", .has_value_range=true, .min_value=0, .max_value=1},
    {.key="vref", .value_hint="1000..5000 mV", .kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role="vref", .has_value_range=true, .min_value=1000, .max_value=5000},
    {.key="divider", .value_hint="1000..10000", .kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role="divider", .has_value_range=true, .min_value=1000, .max_value=10000},
};
const solar_os_expansion_driver_t solar_os_waveshare_io_expansion_driver = {
    .name="waveshare-io", .summary="Waveshare MCU GPIO, dimmer and battery ADC",
    .category=SOLAR_OS_EXPANSION_CATEGORY_UTILITY,
    .required_capabilities=SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
    .probe_supported=true, .early=true, .binding_specs=specs,
    .binding_spec_count=sizeof(specs)/sizeof(specs[0]),
    .attach=solar_os_waveshare_io_attach, .detach=solar_os_waveshare_io_detach,
};
