#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    size_t rx_buffer_size;
    size_t tx_buffer_size;
} cdc_port_config_t;

esp_err_t cdc_port_init(const cdc_port_config_t *config);
esp_err_t cdc_port_deinit(void);
bool cdc_port_is_ready(void);
bool cdc_port_is_connected(void);
bool cdc_port_driver_installed(void);
esp_err_t cdc_port_write(const uint8_t *data, size_t len, size_t *written);
esp_err_t cdc_port_read(uint8_t *data, size_t len, uint32_t timeout_ms, size_t *read_len);
