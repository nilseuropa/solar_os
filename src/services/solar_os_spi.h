#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_bus_types.h"

#define SOLAR_OS_SPI_MAX_CS SOLAR_OS_BUS_SPI_CS_MAX
#define SOLAR_OS_SPI_DEFAULT_SPEED_HZ SOLAR_OS_BUS_SPI_DEFAULT_SPEED_HZ
#define SOLAR_OS_SPI_MAX_SPEED_HZ SOLAR_OS_BUS_SPI_MAX_SPEED_HZ

typedef struct {
    int pin;
    char name[SOLAR_OS_BUS_NAME_MAX];
} solar_os_spi_cs_t;

typedef struct {
    bool available;
    int host;
    char name[SOLAR_OS_BUS_NAME_MAX];
    int sclk_pin;
    int miso_pin;
    int mosi_pin;
    size_t max_transfer_size;
    uint32_t default_speed_hz;
    size_t cs_count;
    solar_os_spi_cs_t cs[SOLAR_OS_SPI_MAX_CS];
} solar_os_spi_status_t;

esp_err_t solar_os_spi_init(void);
esp_err_t solar_os_spi_get_status(solar_os_spi_status_t *status);
esp_err_t solar_os_spi_resolve_cs(const char *name, int *pin);
esp_err_t solar_os_spi_transfer(int cs_pin,
                                uint8_t mode,
                                uint32_t speed_hz,
                                const uint8_t *tx_data,
                                uint8_t *rx_data,
                                size_t len);
