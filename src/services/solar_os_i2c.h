#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_I2C_SCAN_MIN_ADDR 0x03
#define SOLAR_OS_I2C_SCAN_MAX_ADDR 0x77
#define SOLAR_OS_I2C_DEFAULT_BUS "i2c0"

esp_err_t solar_os_i2c_init(void);
uint32_t solar_os_i2c_get_speed_hz(void);
int solar_os_i2c_get_sda_pin(void);
int solar_os_i2c_get_scl_pin(void);
esp_err_t solar_os_i2c_bus_probe(const char *name, uint8_t address);
esp_err_t solar_os_i2c_bus_read_reg(const char *name,
                                    uint8_t address,
                                    uint8_t reg,
                                    uint8_t *data,
                                    size_t len);
esp_err_t solar_os_i2c_bus_write_reg(const char *name,
                                     uint8_t address,
                                     uint8_t reg,
                                     const uint8_t *data,
                                     size_t len);
esp_err_t solar_os_i2c_probe(uint8_t address);
esp_err_t solar_os_i2c_read_reg(uint8_t address, uint8_t reg, uint8_t *data, size_t len);
esp_err_t solar_os_i2c_write_reg(uint8_t address, uint8_t reg, const uint8_t *data, size_t len);
