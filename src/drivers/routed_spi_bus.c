#include "routed_spi_bus.h"

#include "driver/gpio.h"

static bool config_valid(const solar_os_bus_spi_config_t *config)
{
    return config != NULL &&
        config->host >= 0 &&
        config->sclk_pin >= 0 &&
        config->mosi_pin >= 0 &&
        config->miso_pin >= -1 &&
        config->max_transfer_size > 0;
}

esp_err_t solar_os_routed_spi_start(const solar_os_bus_spi_config_t *config,
                                    bool allow_existing,
                                    bool *initialized_here)
{
    if (!config_valid(config) || initialized_here == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const spi_bus_config_t bus_config = {
        .mosi_io_num = config->mosi_pin,
        .miso_io_num = config->miso_pin,
        .sclk_io_num = config->sclk_pin,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = (int)config->max_transfer_size,
    };
    const esp_err_t ret = spi_bus_initialize((spi_host_device_t)config->host,
                                             &bus_config,
                                             SPI_DMA_CH_AUTO);
    if (ret == ESP_ERR_INVALID_STATE && allow_existing) {
        *initialized_here = false;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    *initialized_here = true;
    return ESP_OK;
}

esp_err_t solar_os_routed_spi_stop(const solar_os_bus_spi_config_t *config,
                                   bool initialized_here)
{
    if (!config_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!initialized_here) {
        return ESP_OK;
    }

    const esp_err_t ret = spi_bus_free((spi_host_device_t)config->host);
    if (ret != ESP_OK) {
        return ret;
    }
    (void)gpio_reset_pin((gpio_num_t)config->sclk_pin);
    (void)gpio_reset_pin((gpio_num_t)config->mosi_pin);
    if (config->miso_pin >= 0) {
        (void)gpio_reset_pin((gpio_num_t)config->miso_pin);
    }
    return ESP_OK;
}

esp_err_t solar_os_routed_spi_add_device(const solar_os_bus_spi_config_t *config,
                                         const spi_device_interface_config_t *device_config,
                                         spi_device_handle_t *device)
{
    if (!config_valid(config) || device_config == NULL || device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return spi_bus_add_device((spi_host_device_t)config->host, device_config, device);
}

esp_err_t solar_os_routed_spi_transfer(const solar_os_bus_spi_config_t *config,
                                       int cs_pin,
                                       uint8_t mode,
                                       uint32_t speed_hz,
                                       const uint8_t *tx_data,
                                       uint8_t *rx_data,
                                       size_t len)
{
    if (!config_valid(config) || cs_pin < 0 || mode > 3 || speed_hz == 0 ||
        len == 0 || len > config->max_transfer_size ||
        (tx_data == NULL && rx_data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_device_handle_t device = NULL;
    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = (int)speed_hz,
        .mode = mode,
        .spics_io_num = cs_pin,
        .queue_size = 1,
    };
    esp_err_t ret = solar_os_routed_spi_add_device(config, &device_config, &device);
    if (ret != ESP_OK) {
        return ret;
    }

    spi_transaction_t transaction = {
        .length = len * 8U,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };
    ret = spi_device_transmit(device, &transaction);
    const esp_err_t remove_ret = spi_bus_remove_device(device);
    return ret == ESP_OK ? remove_ret : ret;
}
