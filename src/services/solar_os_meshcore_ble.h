#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_ble.h"
#include "solar_os_native_meshcore_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_MESHCORE_BLE_WORKER_STACK 8192U

esp_err_t solar_os_meshcore_ble_start(const uint8_t bda[6], uint8_t addr_type,
                                      bool pin_supplied, uint32_t pin,
                                      const char *owner);
void solar_os_meshcore_ble_loop_once(void);
void solar_os_meshcore_ble_cancel(void);
void solar_os_meshcore_ble_stop(void);
esp_err_t solar_os_meshcore_ble_get_status(solar_os_meshcore_ble_status_t *status);
void solar_os_meshcore_ble_note_stack_watermark(uint32_t bytes);
const char *solar_os_meshcore_ble_state_name(solar_os_meshcore_ble_state_t state);

#ifdef __cplusplus
}
#endif
