#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "solar_os_bus_types.h"

esp_err_t solar_os_routed_spi_start(const solar_os_bus_spi_config_t *config,
                                    bool allow_existing,
                                    bool *initialized_here);
esp_err_t solar_os_routed_spi_stop(const solar_os_bus_spi_config_t *config,
                                   bool initialized_here);
esp_err_t solar_os_routed_spi_add_device(const solar_os_bus_spi_config_t *config,
                                         const spi_device_interface_config_t *device_config,
                                         spi_device_handle_t *device);
esp_err_t solar_os_routed_spi_transfer(const solar_os_bus_spi_config_t *config,
                                       int cs_pin,
                                       uint8_t mode,
                                       uint32_t speed_hz,
                                       const uint8_t *tx_data,
                                       uint8_t *rx_data,
                                       size_t len);
