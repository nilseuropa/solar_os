#include "solar_os_gpio_keys.h"

const solar_os_expansion_driver_t solar_os_gpio_keys_expansion_driver = {
    .name = "gpio-keys",
    .summary = "GPIO keyboard",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .allow_unlisted_bindings = true,
    .attach = solar_os_gpio_keys_attach,
    .detach = solar_os_gpio_keys_detach,
};
