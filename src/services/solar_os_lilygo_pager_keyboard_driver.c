#include "solar_os_lilygo_pager_keyboard.h"
#include "solar_os_tca8418.h"

static const int addresses[] = {SOLAR_OS_TCA8418_ADDRESS};
static const solar_os_expansion_binding_spec_t bindings[] = {
    {.key="i2c", .value_hint="bus", .kind=SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required=true},
    {.key="addr", .value_hint="0x34", .kind=SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS,
     .required=true, .allowed_values=addresses, .allowed_value_count=1},
    {.key="irq", .value_hint="gpio", .role="irq", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key="reset", .value_hint="gpio", .role="reset", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key="backlight", .value_hint="gpio", .role="backlight", .kind=SOLAR_OS_EXPANSION_BINDING_PWM},
};

const solar_os_expansion_driver_t solar_os_lilygo_pager_keyboard_expansion_driver = {
    .name="lilygo-pager-keyboard",
    .category=SOLAR_OS_EXPANSION_CATEGORY_INPUT,
    .summary="LilyGO T-LoRa-Pager 4x10 keyboard",
    .required_capabilities=SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
    .probe_supported=true,
    .binding_specs=bindings, .binding_spec_count=sizeof(bindings)/sizeof(bindings[0]),
    .attach=solar_os_lilygo_pager_keyboard_attach,
    .detach=solar_os_lilygo_pager_keyboard_detach,
};
