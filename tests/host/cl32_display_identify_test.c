#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/drivers/cl32_display_identify.c"

static int pull, resets;
static uint8_t command;
static unsigned acquired, released, removed;
static bool floating, inconsistent, ambiguous, grounded;
static esp_err_t add_error, transfer_error, remove_error;
static cl32_display_controller_t actual;

void vTaskDelay(TickType_t ticks) { assert(ticks); }
esp_err_t gpio_config(const gpio_config_t *gpio)
{ assert(gpio->mode == GPIO_MODE_OUTPUT); return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{ if (pin == 12 && level) resets++; return ESP_OK; }
esp_err_t gpio_set_pull_mode(gpio_num_t pin, int mode)
{ assert(pin == 10); pull = mode; return ESP_OK; }
esp_err_t solar_os_bus_spi_add_device(const char *bus,
    const spi_device_interface_config_t *config, spi_device_handle_t *spi)
{
    assert(!strcmp(bus, "spi0") && config->spics_io_num == 6);
    assert(config->flags & SPI_DEVICE_3WIRE);
    assert(config->flags & SPI_DEVICE_HALFDUPLEX);
    assert(config->clock_speed_hz == 500000);
    if (add_error != ESP_OK) return add_error;
    *spi = (void *)1; return ESP_OK;
}
esp_err_t spi_device_acquire_bus(spi_device_handle_t spi, unsigned wait)
{ assert(spi && wait); acquired++; return ESP_OK; }
void spi_device_release_bus(spi_device_handle_t spi) { assert(spi); released++; }
esp_err_t spi_bus_remove_device(spi_device_handle_t spi)
{ assert(spi); removed++; return remove_error; }
esp_err_t spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *tx)
{
    assert(spi);
    if (transfer_error != ESP_OK) return transfer_error;
    if (!tx->rxlength) {
        assert(tx->flags & SPI_TRANS_CS_KEEP_ACTIVE);
        command = tx->tx_data[0];
        assert(command == 0x0a || command == 0x2f);
        return ESP_OK;
    }
    uint8_t value = pull == GPIO_PULLUP_ONLY ? 0xff : 0;
    cl32_display_controller_t selected = actual;
    if (inconsistent && resets == 2) selected = CL32_DISPLAY_ST7305;
    if (!floating && !grounded) {
        if (command == 0x0a && (selected == CL32_DISPLAY_ST7305 || ambiguous)) value = 0x08;
        if (command == 0x2f && (selected == CL32_DISPLAY_SSD1685 || ambiguous)) value = 0x01;
    }
    if (grounded) value = 0;
    *(uint8_t *)tx->rx_buffer = value;
    return ESP_OK;
}

static esp_err_t identify(cl32_display_controller_t *result)
{
    acquired = released = removed = resets = 0;
    const esp_err_t err = cl32_display_identify("spi0", 6, 13, 12, 10, result);
    assert(acquired == released);
    assert(removed == (add_error == ESP_OK ? 1U : 0U));
    assert(pull == GPIO_FLOATING);
    if (err != ESP_OK) assert(*result == CL32_DISPLAY_UNKNOWN);
    return err;
}

int main(void)
{
    cl32_display_controller_t result;
    actual = CL32_DISPLAY_ST7305;
    assert(identify(&result) == ESP_OK && result == actual && resets == 2);
    actual = CL32_DISPLAY_SSD1685;
    assert(identify(&result) == ESP_OK && result == actual && resets == 2);
    floating = true;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED);
    floating = false; grounded = true;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED);
    grounded = false; ambiguous = true;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED);
    ambiguous = false; inconsistent = true;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED);
    inconsistent = false; transfer_error = ESP_FAIL;
    assert(identify(&result) == ESP_FAIL);
    transfer_error = ESP_OK; remove_error = ESP_FAIL;
    assert(identify(&result) == ESP_FAIL);
    remove_error = ESP_OK; add_error = ESP_ERR_NO_MEM;
    assert(identify(&result) == ESP_ERR_NO_MEM);
    assert(cl32_display_identify("spi0", 6, 13, 13, 10, &result) == ESP_ERR_INVALID_ARG);
    assert(cl32_display_identify("spi0", 6, 13, 12, 10, NULL) == ESP_ERR_INVALID_ARG);
    puts("CL-32 controller identification tests passed");
}
