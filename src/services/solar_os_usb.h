#pragma once

#include "esp_err.h"
#include "usb_host_port.h"

typedef enum {
    SOLAR_OS_USB_MODE_DEVICE,
    SOLAR_OS_USB_MODE_HOST,
    SOLAR_OS_USB_MODE_SUSPENDED,
    SOLAR_OS_USB_MODE_FAULT,
} solar_os_usb_mode_t;

typedef struct {
    solar_os_usb_mode_t mode;
    bool sleep_prepared;
    /* Commanded switch state on boards with VBUS control; not voltage feedback. */
    bool vbus_enabled;
    esp_err_t last_error;
    esp_err_t rollback_error;
    size_t interface_count;
    usb_host_port_interface_t interfaces[USB_HOST_PORT_INTERFACE_MAX];
} solar_os_usb_status_t;

esp_err_t solar_os_usb_set_mode(solar_os_usb_mode_t mode);
void solar_os_usb_get_status(solar_os_usb_status_t *status);
const char *solar_os_usb_mode_name(solar_os_usb_mode_t mode);
/* Stop USB before explicit sleep; resume the previous host mode on wake or
 * cancelled sleep. Display-only suspend keeps hosting and can wake on input. */
esp_err_t solar_os_usb_prepare_sleep(void);
esp_err_t solar_os_usb_resume(void);
