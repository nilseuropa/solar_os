#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"

typedef struct {
    uart_port_t port_num;
    gpio_num_t tx_pin;
    gpio_num_t rx_pin;
    uint32_t baud_rate;
    size_t rx_buffer_size;
    size_t tx_buffer_size;
} uart_port_config_t;

esp_err_t uart_port_init(const uart_port_config_t *config);
esp_err_t uart_port_deinit(uart_port_t port_num);
bool uart_port_is_ready(uart_port_t port_num);
esp_err_t uart_port_set_baud_rate(uart_port_t port_num, uint32_t baud_rate);
esp_err_t uart_port_write(uart_port_t port_num,
                          const uint8_t *data,
                          size_t len,
                          size_t *written);
esp_err_t uart_port_read(uart_port_t port_num,
                         uint8_t *data,
                         size_t len,
                         uint32_t timeout_ms,
                         size_t *read_len);
esp_err_t uart_port_get_rx_buffered(uart_port_t port_num, size_t *buffered);
bool uart_port_get_config(uart_port_t port_num, uart_port_config_t *config);
