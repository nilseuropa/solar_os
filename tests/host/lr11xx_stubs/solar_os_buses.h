#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t solar_os_bus_spi_transfer(const char *name,
                                    int cs_pin,
                                    uint8_t mode,
                                    uint32_t speed_hz,
                                    const uint8_t *tx_data,
                                    uint8_t *rx_data,
                                    size_t len);
