#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "solar_os_native_usb_abi.h"
#include "solar_os_port.h"

#define SOLAR_OS_CDC_PORT_NAME "cdc0"

esp_err_t solar_os_cdc_init(void);
/* Refuses an owned cdc0; blocks new opens while the driver is stopped. */
esp_err_t solar_os_cdc_deinit(void);
void solar_os_cdc_get_status(solar_os_cdc_status_t *status);
