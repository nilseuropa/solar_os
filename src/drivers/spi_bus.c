#include "spi_bus.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "solar_os_board.h"
#ifdef SOLAR_OS_BOARD_SPI_RESOURCE_OWNER
#include "solar_os_resources.h"
#endif

#ifndef SOLAR_OS_BOARD_SPI_DMA_CH
#define SOLAR_OS_BOARD_SPI_DMA_CH SPI_DMA_CH_AUTO
#endif

#ifndef SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ
#define SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ 4096
#endif

static size_t spi_bus_ref_count;
static bool spi_bus_initialized_by_us;
#ifdef SOLAR_OS_BOARD_SPI_RESOURCE_OWNER
static bool spi_bus_resource_claimed;
#endif

spi_host_device_t solar_os_spi_bus_host(void)
{
    return SOLAR_OS_BOARD_SPI_HOST;
}

int solar_os_spi_bus_sclk_pin(void)
{
    return (int)SOLAR_OS_BOARD_PIN_SPI_SCLK;
}

int solar_os_spi_bus_miso_pin(void)
{
    return (int)SOLAR_OS_BOARD_PIN_SPI_MISO;
}

int solar_os_spi_bus_mosi_pin(void)
{
    return (int)SOLAR_OS_BOARD_PIN_SPI_MOSI;
}

size_t solar_os_spi_bus_max_transfer_size(void)
{
    return SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ;
}

esp_err_t solar_os_spi_bus_acquire(void)
{
    if (spi_bus_ref_count > 0) {
        spi_bus_ref_count++;
        return ESP_OK;
    }

#ifdef SOLAR_OS_BOARD_SPI_RESOURCE_OWNER
    const esp_err_t claim_ret = solar_os_resource_claim(SOLAR_OS_RESOURCE_SPI_HOST,
                                                        SOLAR_OS_BOARD_SPI_HOST,
                                                        -1,
                                                        SOLAR_OS_BOARD_SPI_RESOURCE_OWNER,
                                                        "spi-host");
    if (claim_ret != ESP_OK) {
        return claim_ret;
    }
    spi_bus_resource_claimed = true;
#endif

    const spi_bus_config_t bus_config = {
        .mosi_io_num = SOLAR_OS_BOARD_PIN_SPI_MOSI,
        .miso_io_num = SOLAR_OS_BOARD_PIN_SPI_MISO,
        .sclk_io_num = SOLAR_OS_BOARD_PIN_SPI_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ,
    };
    const esp_err_t err = spi_bus_initialize(SOLAR_OS_BOARD_SPI_HOST,
                                             &bus_config,
                                             SOLAR_OS_BOARD_SPI_DMA_CH);
#ifdef SOLAR_OS_BOARD_SPI_RESOURCE_OWNER
    if (err == ESP_ERR_INVALID_STATE) {
        (void)solar_os_resource_release(SOLAR_OS_RESOURCE_SPI_HOST,
                                        SOLAR_OS_BOARD_SPI_HOST,
                                        -1,
                                        SOLAR_OS_BOARD_SPI_RESOURCE_OWNER);
        spi_bus_resource_claimed = false;
        return err;
    }
#endif
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
#ifdef SOLAR_OS_BOARD_SPI_RESOURCE_OWNER
        (void)solar_os_resource_release(SOLAR_OS_RESOURCE_SPI_HOST,
                                        SOLAR_OS_BOARD_SPI_HOST,
                                        -1,
                                        SOLAR_OS_BOARD_SPI_RESOURCE_OWNER);
        spi_bus_resource_claimed = false;
#endif
        return err;
    }

    spi_bus_ref_count = 1;
    spi_bus_initialized_by_us = err == ESP_OK;
    return ESP_OK;
}

void solar_os_spi_bus_release(void)
{
    if (spi_bus_ref_count == 0) {
        return;
    }

    spi_bus_ref_count--;
    if (spi_bus_ref_count != 0) {
        return;
    }

    if (spi_bus_initialized_by_us) {
        (void)spi_bus_free(SOLAR_OS_BOARD_SPI_HOST);
    }
    spi_bus_initialized_by_us = false;
#ifdef SOLAR_OS_BOARD_SPI_RESOURCE_OWNER
    if (spi_bus_resource_claimed) {
        (void)solar_os_resource_release(SOLAR_OS_RESOURCE_SPI_HOST,
                                        SOLAR_OS_BOARD_SPI_HOST,
                                        -1,
                                        SOLAR_OS_BOARD_SPI_RESOURCE_OWNER);
        spi_bus_resource_claimed = false;
    }
#endif
}

esp_err_t solar_os_spi_bus_transfer(int cs_pin,
                                    uint8_t mode,
                                    uint32_t speed_hz,
                                    const uint8_t *tx_data,
                                    uint8_t *rx_data,
                                    size_t len)
{
    if (cs_pin < 0 || mode > 3 || speed_hz == 0 || len == 0 ||
        len > SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ ||
        (tx_data == NULL && rx_data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = solar_os_spi_bus_acquire();
    if (err != ESP_OK) {
        return err;
    }

    spi_device_handle_t device = NULL;
    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = (int)speed_hz,
        .mode = mode,
        .spics_io_num = cs_pin,
        .queue_size = 1,
    };

    err = spi_bus_add_device(SOLAR_OS_BOARD_SPI_HOST, &device_config, &device);
    if (err == ESP_OK) {
        spi_transaction_t transaction = {
            .length = len * 8U,
            .tx_buffer = tx_data,
            .rx_buffer = rx_data,
        };
        err = spi_device_transmit(device, &transaction);
        const esp_err_t remove_err = spi_bus_remove_device(device);
        if (err == ESP_OK) {
            err = remove_err;
        }
    }

    solar_os_spi_bus_release();
    return err;
}
