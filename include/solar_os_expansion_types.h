#pragma once

#include <stdint.h>

#define SOLAR_OS_EXPANSION_SPI_CS_MAX 4

typedef struct {
    const char *name;
    int pin;
} solar_os_expansion_pin_t;

typedef struct {
    const char *name;
    int port;
    int sda_pin;
    int scl_pin;
} solar_os_expansion_i2c_bus_t;

typedef struct {
    const char *name;
    int host;
    int sclk_pin;
    int miso_pin;
    int mosi_pin;
    uint32_t max_transfer_size;
    uint8_t cs_count;
    solar_os_expansion_pin_t cs[SOLAR_OS_EXPANSION_SPI_CS_MAX];
} solar_os_expansion_spi_bus_t;

typedef struct {
    const char *name;
    int port;
    int tx_pin;
    int rx_pin;
} solar_os_expansion_uart_port_t;
