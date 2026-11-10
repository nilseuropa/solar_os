#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t sector_count, sector_size;
    uint16_t vendor_id, product_id;
    uint32_t media_revision;
    bool ufi_cbi, media_present, write_protected;
} solar_os_usb_msc_info_t;

/* Task-level callback: queue work; never perform device or filesystem I/O here.
 * One BOT/SCSI or UFI/CBI drive is supported. Callers serialize I/O. */
typedef void (*solar_os_usb_msc_callback_t)(bool connected, uint8_t address);
esp_err_t solar_os_usb_msc_start(solar_os_usb_msc_callback_t callback);
esp_err_t solar_os_usb_msc_stop(void);
esp_err_t solar_os_usb_msc_open(uint8_t address, solar_os_usb_msc_info_t *info);
esp_err_t solar_os_usb_msc_close(void);
bool solar_os_usb_msc_present(void);
esp_err_t solar_os_usb_msc_refresh(solar_os_usb_msc_info_t *info);
bool solar_os_usb_msc_media_current(const solar_os_usb_msc_info_t *info);
esp_err_t solar_os_usb_msc_read(uint32_t sector, void *data, size_t bytes);
esp_err_t solar_os_usb_msc_write(uint32_t sector, const void *data, size_t bytes);
