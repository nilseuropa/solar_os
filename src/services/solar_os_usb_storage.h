#pragma once

#include "solar_os_native_usb_abi.h"
#include "solar_os_storage.h"

#define SOLAR_OS_USB_STORAGE_MOUNT_POINT "/usb0"

/* The USB host library must remain running until stop succeeds. */
esp_err_t solar_os_usb_storage_init(void);
esp_err_t solar_os_usb_storage_start(void);
esp_err_t solar_os_usb_storage_stop(void);
esp_err_t solar_os_usb_storage_mount(void);
esp_err_t solar_os_usb_storage_unmount(void);
void solar_os_usb_storage_get_status(solar_os_usb_storage_status_t *status);
bool solar_os_usb_storage_get_block(solar_os_storage_block_t *block);
esp_err_t solar_os_usb_storage_get_usage(solar_os_storage_usage_t *usage);
