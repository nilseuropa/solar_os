#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

/* XL9555 default 7-bit I2C address (A0=A1=A2=GND). */
#define SOLAR_OS_TLORA_PAGER_CORE_ADDRESS 0x20

esp_err_t solar_os_tlora_pager_core_attach(const char *name,
                                           const solar_os_expansion_binding_t *bindings,
                                           size_t binding_count);
esp_err_t solar_os_tlora_pager_core_detach(const char *name);

/* Toggle individual power rails on the XL9555. */
esp_err_t solar_os_tlora_pager_core_set_gnss_power(bool on);
bool solar_os_tlora_pager_core_get_gnss_power(void);
esp_err_t solar_os_tlora_pager_core_set_nfc_power(bool on);
bool solar_os_tlora_pager_core_get_nfc_power(void);
