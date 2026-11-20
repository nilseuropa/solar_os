#include "solar_os_qemu_eth.h"

const solar_os_expansion_driver_t solar_os_qemu_eth_expansion_driver = {
    .name = "qemu-eth",
    .category = SOLAR_OS_EXPANSION_CATEGORY_RADIO,
    .summary = "QEMU virtual Ethernet (open_eth)",
    .required_capabilities = SOLAR_OS_BOARD_CAP_WIFI,
    .binding_specs = NULL,
    .binding_spec_count = 0,
    .attach = solar_os_qemu_eth_attach,
    .detach = solar_os_qemu_eth_detach,
};
