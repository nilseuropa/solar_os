#pragma once

#include "esp_err.h"
#include "usb_host_port.h"
#include "solar_os_native_usb_abi.h"

/* Apply the board's volatile boot policy once. */
esp_err_t solar_os_usb_init(void);
esp_err_t solar_os_usb_set_mode(solar_os_usb_mode_t mode);
void solar_os_usb_get_status(solar_os_usb_status_t *status);
const char *solar_os_usb_mode_name(solar_os_usb_mode_t mode);
/* Stop USB before explicit sleep; resume the previous host mode on wake or
 * cancelled sleep. Display-only suspend keeps hosting and can wake on input. */
esp_err_t solar_os_usb_prepare_sleep(void);
esp_err_t solar_os_usb_resume(void);
