#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef void *spi_device_handle_t;
typedef struct {
    int spics_io_num, clock_speed_hz, mode, queue_size;
    unsigned flags;
} spi_device_interface_config_t;

#define SPI_DEVICE_BIT_LSBFIRST 3U
#define SPI_DEVICE_3WIRE (1U << 2)
#define SPI_DEVICE_HALFDUPLEX (1U << 4)
#define SPI_DEVICE_NO_DUMMY (1U << 6)
#define SPI_TRANS_VARIABLE_CMD (1U << 5)
#define SPI_TRANS_VARIABLE_ADDR (1U << 6)
#define SPI_TRANS_VARIABLE_DUMMY (1U << 7)

typedef struct {
    size_t length, rxlength;
    unsigned flags;
    uint16_t cmd;
    uint64_t addr;
    const void *tx_buffer;
    void *rx_buffer;
} spi_transaction_t;

typedef struct {
    spi_transaction_t base;
    uint8_t command_bits, address_bits, dummy_bits;
} spi_transaction_ext_t;

esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *transaction);
esp_err_t spi_bus_remove_device(spi_device_handle_t device);
