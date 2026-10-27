#include "solar_os_pcnt_encoder.h"

static const int modes[] = {1, 2, 4};
static const int booleans[] = {0, 1};
static const int pulls[] = {0, 1, 2};
static const solar_os_expansion_binding_spec_t bindings[] = {
    {.key = "a", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "a", .required = true},
    {.key = "b", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "b", .required = true},
    {.key = "mode", .value_hint = "1|2|4", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "mode", .required = true,
        .allowed_values = modes, .allowed_value_count = 3},
    {.key = "invert", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "invert",
        .allowed_values = booleans, .allowed_value_count = 2},
    {.key = "pull", .value_hint = "0|1|2", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "pull",
        .allowed_values = pulls, .allowed_value_count = 3},
    {.key = "filter_ns", .value_hint = "1000", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "filter_ns",
        .has_value_range = true, .min_value = 0, .max_value = INT32_MAX},
};

const solar_os_expansion_driver_t solar_os_pcnt_encoder_expansion_driver = {
    .name = "pcnt-encoder", .category = SOLAR_OS_EXPANSION_CATEGORY_SENSOR,
    .summary = "Hardware quadrature encoder with explicit x1/x2/x4 counting",
    .required_capabilities = SOLAR_OS_BOARD_CAP_GPIO, .probe_supported = false,
    .binding_specs = bindings, .binding_spec_count = sizeof(bindings) / sizeof(bindings[0]),
    .attach = solar_os_pcnt_encoder_attach, .detach = solar_os_pcnt_encoder_detach,
    .validate_bindings = solar_os_pcnt_encoder_validate,
};
