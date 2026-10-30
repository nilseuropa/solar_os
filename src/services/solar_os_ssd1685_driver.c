#include "solar_os_ssd1685.h"
#include "epd_ssd1685.h"

static const int panels[] = {EPD_SSD1685_PANEL_GDEY029T71H};


static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
    {.key = "dc", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc", .required = true},
    {.key = "reset", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .required = true},
    {.key = "busy", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "busy", .required = true},
    {.key = "power", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "power"},
    {.key = "panel", .value_hint = "1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "panel", .required = true, .allowed_values = panels, .allowed_value_count = sizeof(panels) / sizeof(panels[0])},
    {.key = "clock", .value_hint = "khz", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "clock", .has_value_range = true, .min_value = 100, .max_value = 10000},
    {.key = "rotation", .value_hint = "0..3", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "rotation", .has_value_range = true, .min_value = 0, .max_value = 3},
};

const solar_os_expansion_driver_t solar_os_ssd1685_expansion_driver = {
    .name = "ssd1685",
    .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .summary = "SSD1685 GDEY029T71H 168x384 e-paper",
    .required_capabilities = SOLAR_OS_BOARD_CAP_GFX |
                             SOLAR_OS_BOARD_CAP_EXPANSION_SPI |
                             SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_ssd1685_attach,
    .detach = solar_os_ssd1685_detach,
};
