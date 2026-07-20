#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_ONEWIRE_MAX_DEVICES 32U
#define SOLAR_OS_ONEWIRE_MAX_TRANSFER 64U

esp_err_t solar_os_onewire_init(void);
bool solar_os_onewire_pin_allowed(int pin);
esp_err_t solar_os_onewire_reset(int pin, bool *present);
esp_err_t solar_os_onewire_reset_configured(int pin, bool *present);
esp_err_t solar_os_onewire_scan(int pin,
                                uint64_t *addresses,
                                size_t max_addresses,
                                size_t *address_count);
esp_err_t solar_os_onewire_scan_configured(int pin,
                                           uint64_t *addresses,
                                           size_t max_addresses,
                                           size_t *address_count);
esp_err_t solar_os_onewire_transfer(int pin,
                                    const uint8_t *tx_data,
                                    size_t tx_len,
                                    uint8_t *rx_data,
                                    size_t rx_len);
esp_err_t solar_os_onewire_transfer_configured(int pin,
                                               const uint8_t *tx_data,
                                               size_t tx_len,
                                               uint8_t *rx_data,
                                               size_t rx_len);
esp_err_t solar_os_onewire_bus_reset(const char *name, bool *present);
esp_err_t solar_os_onewire_bus_scan(const char *name,
                                    uint64_t *addresses,
                                    size_t max_addresses,
                                    size_t *address_count);
esp_err_t solar_os_onewire_bus_transfer(const char *name,
                                        const uint8_t *tx_data,
                                        size_t tx_len,
                                        uint8_t *rx_data,
                                        size_t rx_len);
