#include "solar_os_audio_pdm.h"

static const int ports[] = {0};
static const solar_os_expansion_binding_spec_t specs[] = {
    {.key="i2s", .value_hint="i2s0", .kind=SOLAR_OS_EXPANSION_BINDING_I2S_PORT,
        .required=true, .allowed_values=ports, .allowed_value_count=1},
    {.key="clk", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="clk", .required=true},
    {.key="din", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="din", .required=true},
    {.key="power", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="power"},
    {.key="slot", .value_hint="0|1", .kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role="slot",
        .has_value_range=true, .min_value=0, .max_value=1},
};
const solar_os_expansion_driver_t solar_os_audio_pdm_expansion_driver = {
    .name="audio-pdm", .category=SOLAR_OS_EXPANSION_CATEGORY_AUDIO,
    .summary="PDM mono microphone capture", .required_capabilities=SOLAR_OS_BOARD_CAP_EXPANSION_I2S,
    .binding_specs=specs, .binding_spec_count=sizeof(specs)/sizeof(specs[0]),
    .attach=solar_os_audio_pdm_attach, .detach=solar_os_audio_pdm_detach,
};
