#pragma once

#include "usb_ufi.h"

/* Hooks for the generated copy of pinned usb_host_msc 1.3.0. The managed
 * component's private structures and ownership remain unchanged. */
esp_err_t solar_os_usb_ufi_msc_bind(void *device);
void solar_os_usb_ufi_msc_unbind(void *device);
solar_os_usb_ufi_t *solar_os_usb_ufi_msc_state(void *device);
esp_err_t solar_os_usb_ufi_msc_initialize(void *device, uint32_t *size, uint32_t *count);
esp_err_t solar_os_usb_ufi_msc_execute(void *device, const uint8_t *command, size_t length,
                                      bool input, void *data, size_t size);
esp_err_t solar_os_usb_ufi_msc_endpoint(void *device, uint8_t endpoint, uint16_t mps,
                                       void *data, size_t size, size_t *actual);
