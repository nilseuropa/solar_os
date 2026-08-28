#include "solar_os_audio_pwm.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "pwm", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_PWM, .role = "pwm", .required = true},
};

const solar_os_expansion_driver_t solar_os_audio_pwm_expansion_driver = {
    .name = "audio-pwm",
    .summary = "PWM mono audio",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_PWM,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_audio_pwm_attach,
    .detach = solar_os_audio_pwm_detach,
};
