#pragma once

#include "solar_os_storage.h"

#define SOLAR_OS_USB_STORAGE_MOUNT_POINT "/usb0"

typedef struct {
    bool connected, mounted;
    bool ufi_cbi, media_present, write_protected;
    uint8_t address;
    uint16_t vendor_id, product_id;
    uint32_t sector_size, sector_count;
    size_t open_handles;
    esp_err_t last_error;
} solar_os_usb_storage_status_t;

/* The USB host library must remain running until stop succeeds. */
esp_err_t solar_os_usb_storage_init(void);
esp_err_t solar_os_usb_storage_start(void);
esp_err_t solar_os_usb_storage_stop(void);
esp_err_t solar_os_usb_storage_mount(void);
esp_err_t solar_os_usb_storage_unmount(void);
void solar_os_usb_storage_get_status(solar_os_usb_storage_status_t *status);
bool solar_os_usb_storage_get_block(solar_os_storage_block_t *block);
esp_err_t solar_os_usb_storage_get_usage(solar_os_storage_usage_t *usage);
