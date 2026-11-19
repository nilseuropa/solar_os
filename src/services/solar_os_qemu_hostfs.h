#pragma once

#include "solar_os_storage.h"

#define SOLAR_OS_QEMU_HOSTFS_MOUNT "/host"
esp_err_t solar_os_qemu_hostfs_mount(void);
esp_err_t solar_os_qemu_hostfs_unmount(void);
bool solar_os_qemu_hostfs_is_mounted(void);
esp_err_t solar_os_qemu_hostfs_get_usage(solar_os_storage_usage_t *usage);
