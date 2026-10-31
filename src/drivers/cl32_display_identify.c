#include "cl32_display_identify.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

static void delay_ms(unsigned ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t read_register(spi_device_handle_t spi, int dc, uint8_t reg, uint8_t *value)
{
    esp_err_t err = spi_device_acquire_bus(spi, portMAX_DELAY);
    if (err != ESP_OK) return err;
    err = gpio_set_level(dc, 0);
    spi_transaction_t tx = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_CS_KEEP_ACTIVE,
        .length = 8, .tx_data = {reg},
    };
    if (err == ESP_OK) err = spi_device_polling_transmit(spi, &tx);
    if (err == ESP_OK) err = gpio_set_level(dc, 1);
    spi_transaction_t rx = {.rxlength = 8, .rx_buffer = value};
    if (err == ESP_OK) err = spi_device_polling_transmit(spi, &rx);
    spi_device_release_bus(spi);
    return err;
}

esp_err_t cl32_display_identify(const char *spi_bus, int cs, int dc, int reset,
                               int mosi, cl32_display_controller_t *controller)
{
    if (controller == NULL) return ESP_ERR_INVALID_ARG;
    *controller = CL32_DISPLAY_UNKNOWN;
    if (spi_bus == NULL || spi_bus[0] == '\0') return ESP_ERR_INVALID_ARG;
    const int pins[] = {cs, dc, reset, mosi};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pins[i])) return ESP_ERR_INVALID_ARG;
        for (size_t j = 0; j < i; j++) {
            if (pins[i] == pins[j]) return ESP_ERR_INVALID_ARG;
        }
    }
    const gpio_config_t output = {
        .pin_bit_mask = (1ULL << dc) | (1ULL << reset), .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&output);
    if (err != ESP_OK) return err;
    const spi_device_interface_config_t config = {
        .clock_speed_hz = 500000, .mode = 0, .spics_io_num = cs, .queue_size = 1,
        .flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
    };
    spi_device_handle_t spi = NULL;
    err = solar_os_bus_spi_add_device(spi_bus, &config, &spi);
    cl32_display_controller_t previous = CL32_DISPLAY_UNKNOWN;
    for (unsigned pass = 0; err == ESP_OK && pass < 2; pass++) {
        /* Opposite weak pulls reject undriven SDA readback. Both controllers
         * return data on SDA/MOSI, not the SD card's shared MISO line. */
        /* Keep the SPI matrix routing intact while changing only the pulls. */
        err = gpio_set_pull_mode(mosi, pass == 0 ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
        if (err == ESP_OK) err = gpio_set_level(reset, 0);
        delay_ms(20);
        if (err == ESP_OK) err = gpio_set_level(reset, 1);
        delay_ms(100);
        uint8_t power = 0, status = 0;
        if (err == ESP_OK) err = read_register(spi, dc, 0x0a, &power);
        if (err == ESP_OK) err = read_register(spi, dc, 0x2f, &status);
        /* ST7305 RDDPM POR=08 (normal, asleep, display off).
         * SSD1685 status POR=01 (chip ID=1, not busy).
         * See ST7305 v0.2 8.1.5 and SSD1685 rev0.11 command 2Fh. */
        const bool rlcd = power == 0x08;
        const bool epd = status == 0x01;
        const cl32_display_controller_t selected = rlcd == epd ? CL32_DISPLAY_UNKNOWN :
            rlcd ? CL32_DISPLAY_ST7305 : CL32_DISPLAY_SSD1685;
        if (err == ESP_OK && (selected == CL32_DISPLAY_UNKNOWN ||
                             (pass != 0 && selected != previous))) {
            err = ESP_ERR_NOT_SUPPORTED;
        }
        previous = selected;
    }
    if (spi != NULL) {
        const esp_err_t removed = spi_bus_remove_device(spi);
        if (err == ESP_OK) err = removed;
    }
    const esp_err_t restored = gpio_set_pull_mode(mosi, GPIO_FLOATING);
    if (err == ESP_OK) err = restored;
    if (err == ESP_OK) *controller = previous;
    return err;
}
