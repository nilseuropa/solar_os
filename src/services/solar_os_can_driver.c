#include "solar_os_can.h"

static const int rates[]={25,50,100,125,250,500,800,1000};
static const solar_os_expansion_binding_spec_t specs[]={
    {.key="tx",.value_hint="gpio",.kind=SOLAR_OS_EXPANSION_BINDING_GPIO,.role="tx",.required=true},
    {.key="rx",.value_hint="gpio",.kind=SOLAR_OS_EXPANSION_BINDING_GPIO,.role="rx",.required=true},
    {.key="bitrate",.value_hint="25|50|100|125|250|500|800|1000 kbit/s",.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER,.role="bitrate",.required=true,.allowed_values=rates,.allowed_value_count=8},
    {.key="listen",.value_hint="0|1",.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER,.role="listen",.has_value_range=true,.min_value=0,.max_value=1},
    {.key="select",.value_hint="gpio|controller:line",.kind=SOLAR_OS_EXPANSION_BINDING_GPIO_LINE,.role="select"},
    {.key="active",.value_hint="0|1",.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER,.role="active",.has_value_range=true,.min_value=0,.max_value=1},
};
const solar_os_expansion_driver_t solar_os_twai_can_expansion_driver={
    .name="twai-can",.summary="ESP32 TWAI classic CAN controller",.category=SOLAR_OS_EXPANSION_CATEGORY_UTILITY,
    .required_capabilities=SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,.binding_specs=specs,
    .binding_spec_count=sizeof(specs)/sizeof(specs[0]),.attach=solar_os_can_attach,.detach=solar_os_can_detach,
};
