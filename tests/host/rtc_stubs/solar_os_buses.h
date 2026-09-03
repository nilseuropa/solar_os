#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t solar_os_bus_i2c_read_reg(const char *name,
                                    uint8_t address,
                                    uint8_t reg,
                                    uint8_t *data,
                                    size_t len);
esp_err_t solar_os_bus_i2c_write_reg(const char *name,
                                     uint8_t address,
                                     uint8_t reg,
                                     const uint8_t *data,
                                     size_t len);
