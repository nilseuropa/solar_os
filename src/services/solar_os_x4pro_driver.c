#include "solar_os_x4pro.h"

static const solar_os_expansion_binding_spec_t specs[] = {
    {.key="spi", .value_hint="bus", .kind=SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required=true},
    {.key="cs", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required=true},
    {.key="dc", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="dc", .required=true},
    {.key="reset", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="reset", .required=true},
    {.key="busy", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="busy", .required=true},
    {.key="latch", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="latch", .required=true},
    {.key="cool", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="cool", .required=true},
    {.key="warm", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="warm", .required=true},
};

const solar_os_expansion_driver_t solar_os_x4pro_expansion_driver = {
    .name="x4pro", .summary="Xteink X4 Pro display and frontlight",
    .category=SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .board_only=true,
    .required_capabilities=SOLAR_OS_BOARD_CAP_GFX |
        SOLAR_OS_BOARD_CAP_EXPANSION_SPI | SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .early=true, .binding_specs=specs, .binding_spec_count=sizeof(specs)/sizeof(specs[0]),
    .attach=solar_os_x4pro_attach, .detach=solar_os_x4pro_detach,
};
