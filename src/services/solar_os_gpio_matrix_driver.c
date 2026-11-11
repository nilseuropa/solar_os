#include "solar_os_gpio_matrix.h"

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "row0", .value_hint = "gpio", .role = "row0", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .required = true},
    {.key = "row1", .value_hint = "gpio", .role = "row1", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "row2", .value_hint = "gpio", .role = "row2", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "row3", .value_hint = "gpio", .role = "row3", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "row4", .value_hint = "gpio", .role = "row4", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "row5", .value_hint = "gpio", .role = "row5", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "row6", .value_hint = "gpio", .role = "row6", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "row7", .value_hint = "gpio", .role = "row7", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "col0", .value_hint = "gpio", .role = "col0", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .required = true},
    {.key = "col1", .value_hint = "gpio", .role = "col1", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "col2", .value_hint = "gpio", .role = "col2", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "col3", .value_hint = "gpio", .role = "col3", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "col4", .value_hint = "gpio", .role = "col4", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "col5", .value_hint = "gpio", .role = "col5", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "col6", .value_hint = "gpio", .role = "col6", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "col7", .value_hint = "gpio", .role = "col7", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "col8", .value_hint = "gpio", .role = "col8", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "col9", .value_hint = "gpio", .role = "col9", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
    {.key = "drive", .value_hint = "0=rows,1=columns", .role = "drive", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .has_value_range = true, .min_value = 0, .max_value = 1},
    {.key = "diodes", .value_hint = "0=absent,1=present", .role = "diodes", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .has_value_range = true, .min_value = 0, .max_value = 1},
    {.key = "debounce-ms", .value_hint = "1..100", .role = "debounce-ms", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .has_value_range = true, .min_value = 1, .max_value = 100},
};

const solar_os_expansion_driver_t solar_os_gpio_matrix_expansion_driver = {
    .name = "gpio-matrix",
    .category = SOLAR_OS_EXPANSION_CATEGORY_INPUT,
    .summary = "Direct GPIO keyboard matrix",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_gpio_matrix_attach,
    .detach = solar_os_gpio_matrix_detach,
};
