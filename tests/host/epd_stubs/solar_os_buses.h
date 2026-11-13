#pragma once
#include "driver/spi_master.h"
#include "solar_os_bus_types.h"
bool solar_os_bus_find(const char *name, solar_os_bus_protocol_t protocol,
                       solar_os_bus_info_t *info);
esp_err_t solar_os_bus_i2c_transmit_receive(const char *name, uint8_t address,
                                           const uint8_t *tx, size_t tx_len,
                                           uint8_t *rx, size_t rx_len);
esp_err_t solar_os_bus_i2c_transmit(const char *name, uint8_t address,
                                   const uint8_t *tx, size_t tx_len);
esp_err_t solar_os_bus_spi_add_device(const char *name,
                                     const spi_device_interface_config_t *config,
                                     spi_device_handle_t *device);
