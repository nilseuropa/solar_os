#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#define SOLAR_I2C_SPEED_HZ 100000U
#define SOLAR_I2C_SCAN_MIN_ADDR 0x03
#define SOLAR_I2C_SCAN_MAX_ADDR 0x77

typedef struct {
    int port;
    gpio_num_t sda_pin;
    gpio_num_t scl_pin;
    uint32_t speed_hz;
} i2c_bus_config_t;

esp_err_t i2c_bus_init(void);
esp_err_t i2c_bus_init_config(const i2c_bus_config_t *config);
esp_err_t i2c_bus_start_config(const i2c_bus_config_t *config,
                               bool allow_existing,
                               i2c_master_bus_handle_t *handle,
                               bool *initialized_here);
esp_err_t i2c_bus_stop_config(const i2c_bus_config_t *config,
                              i2c_master_bus_handle_t handle,
                              bool initialized_here);
i2c_master_bus_handle_t i2c_bus_get_handle(void);
void i2c_bus_lock(void);
void i2c_bus_unlock(void);
uint32_t i2c_bus_get_speed_hz(void);
gpio_num_t i2c_bus_get_sda_pin(void);
gpio_num_t i2c_bus_get_scl_pin(void);
esp_err_t i2c_bus_probe(uint8_t address);
esp_err_t i2c_bus_probe_handle(i2c_master_bus_handle_t handle, uint8_t address);
esp_err_t i2c_bus_transmit(uint8_t address, const uint8_t *data, size_t len);
esp_err_t i2c_bus_receive(uint8_t address, uint8_t *data, size_t len);
esp_err_t i2c_bus_transmit_receive(uint8_t address,
                                   const uint8_t *tx_data,
                                   size_t tx_len,
                                   uint8_t *rx_data,
                                   size_t rx_len);
esp_err_t i2c_bus_read_reg(uint8_t address, uint8_t reg, uint8_t *data, size_t len);
esp_err_t i2c_bus_write_reg(uint8_t address, uint8_t reg, const uint8_t *data, size_t len);
esp_err_t i2c_bus_read_reg_handle(i2c_master_bus_handle_t handle,
                                  uint32_t speed_hz,
                                  uint8_t address,
                                  uint8_t reg,
                                  uint8_t *data,
                                  size_t len);
esp_err_t i2c_bus_write_reg_handle(i2c_master_bus_handle_t handle,
                                   uint32_t speed_hz,
                                   uint8_t address,
                                   uint8_t reg,
                                   const uint8_t *data,
                                   size_t len);
