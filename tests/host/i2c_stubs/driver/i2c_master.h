#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define I2C_NUM_MAX 2
#define I2C_ADDR_BIT_LEN_7 7
#define I2C_CLK_SRC_DEFAULT 0
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct {
    int i2c_port;
    int sda_io_num;
    int scl_io_num;
    int clk_source;
    int glitch_ignore_cnt;
    struct { bool enable_internal_pullup; } flags;
} i2c_master_bus_config_t;
typedef struct {
    int dev_addr_length;
    uint8_t device_address;
    uint32_t scl_speed_hz;
} i2c_device_config_t;
typedef struct {
    const uint8_t *write_buffer;
    size_t buffer_size;
} i2c_master_transmit_multi_buffer_info_t;

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *, i2c_master_bus_handle_t *);
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t,
                                    const i2c_device_config_t *, i2c_master_dev_handle_t *);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t, uint8_t, int);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t *, size_t, int);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t, uint8_t *, size_t, int);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t,
                                     const uint8_t *, size_t, uint8_t *, size_t, int);
esp_err_t i2c_master_multi_buffer_transmit(i2c_master_dev_handle_t,
                                          const i2c_master_transmit_multi_buffer_info_t *,
                                          size_t, int);
