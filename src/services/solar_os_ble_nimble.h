#pragma once

#include "solar_os_ble_backend.h"
#include "host/ble_hs.h"

/* Only transport files include this header. Public addresses remain in display
 * order; NimBLE uses little-endian addresses. */
void solar_os_ble_nimble_address(ble_addr_t *out, const uint8_t bda[6], uint8_t type);
void solar_os_ble_nimble_display_address(uint8_t out[6], const ble_addr_t *addr);
esp_err_t solar_os_ble_nimble_error(int status);
int solar_os_ble_nimble_security(struct ble_gap_event *event);
int solar_os_ble_nimble_security_passkey(struct ble_gap_event *event,
                                         uint32_t *display_passkey);
bool solar_os_ble_nimble_client_idle(void);
void solar_os_ble_nimble_host_stopped(void);
