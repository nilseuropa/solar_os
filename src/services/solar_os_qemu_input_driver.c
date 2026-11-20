#include "solar_os_qemu_input.h"

const solar_os_expansion_driver_t solar_os_qemu_input_expansion_driver = {
    .name = "qemu-input",
    .category = SOLAR_OS_EXPANSION_CATEGORY_INPUT,
    .summary = "QEMU virtual keyboard, touch, and mouse",
    .required_capabilities = SOLAR_OS_BOARD_CAP_DISPLAY | SOLAR_OS_BOARD_CAP_GFX,
    .attach = solar_os_qemu_input_attach,
    .detach = solar_os_qemu_input_detach,
};
