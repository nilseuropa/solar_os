#include "cl32_display_identify.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
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

static esp_err_t probe_busy_reset(spi_device_handle_t spi, int dc, int busy, bool *response)
{
    esp_err_t err = gpio_set_pull_mode(busy, GPIO_PULLUP_ONLY);
    if (err != ESP_OK) return err;
    err = spi_device_acquire_bus(spi, portMAX_DELAY);
    if (err != ESP_OK) return err;
    err = gpio_set_level(dc, 0);
    /* 12h is SSD1685 software reset (BUSY pulse), but ST7305 partial mode
     * (no BUSY pulse while asleep). Neither command drives a display refresh. */
    spi_transaction_t tx = {.flags = SPI_TRANS_USE_TXDATA,
        .length = 8, .tx_data = {0x12}};
    if (err == ESP_OK) err = spi_device_polling_transmit(spi, &tx);
    spi_device_release_bus(spi);
    if (err != ESP_OK) return err;
    *response = false;
    const int64_t deadline = esp_timer_get_time() + 20000;
    do {
        if (gpio_get_level(busy)) *response = true;
        /* A reset pulse can be shorter than one FreeRTOS tick. */
        esp_rom_delay_us(50);
    } while (esp_timer_get_time() < deadline);
    const int64_t ready_deadline = esp_timer_get_time() + 1000000;
    while (gpio_get_level(busy)) {
        if (esp_timer_get_time() >= ready_deadline) return ESP_ERR_TIMEOUT;
        delay_ms(2);
    }
    return ESP_OK;
}

esp_err_t cl32_display_identify(const char *spi_bus, int cs, int dc, int reset,
                               int mosi, int busy, cl32_display_controller_t *controller)
{
    if (controller == NULL) return ESP_ERR_INVALID_ARG;
    *controller = CL32_DISPLAY_UNKNOWN;
    if (spi_bus == NULL || spi_bus[0] == '\0') return ESP_ERR_INVALID_ARG;
    const int pins[] = {cs, dc, reset, mosi, busy};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if (pins[i] < 0 || pins[i] >= GPIO_NUM_MAX ||
            (i == 4 ? !GPIO_IS_VALID_GPIO(pins[i]) : !GPIO_IS_VALID_OUTPUT_GPIO(pins[i])))
            return ESP_ERR_INVALID_ARG;
        for (size_t j = 0; j < i; j++) {
            if (pins[i] == pins[j]) return ESP_ERR_INVALID_ARG;
        }
    }
    const gpio_config_t output = {
        .pin_bit_mask = (1ULL << dc) | (1ULL << reset), .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&output);
    if (err != ESP_OK) return err;
    const gpio_config_t input = {.pin_bit_mask = 1ULL << busy, .mode = GPIO_MODE_INPUT};
    err = gpio_config(&input);
    if (err != ESP_OK) return err;
    const spi_device_interface_config_t config = {
        .clock_speed_hz = 500000, .mode = 0, .spics_io_num = cs, .queue_size = 1,
        .flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
    };
    spi_device_handle_t spi = NULL;
    err = solar_os_bus_spi_add_device(spi_bus, &config, &spi);
    uint8_t power[2] = {0}, status[2] = {0};
    int busy_reset[2] = {0}, busy_ready[2] = {0};
    cl32_display_controller_t selected = CL32_DISPLAY_UNKNOWN;
    for (unsigned pass = 0; err == ESP_OK && pass < 2; pass++) {
        /* Controller readback uses SDA/MOSI, not the SD card's MISO line.
         * Some CL-32 RLCD modules do not expose usable register readback. */
        /* Keep the SPI matrix routing intact while changing only the pulls. */
        err = gpio_set_pull_mode(mosi, pass == 0 ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
        if (err == ESP_OK) err = gpio_set_pull_mode(busy, pass == 0 ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
        if (err == ESP_OK) err = gpio_set_level(reset, 0);
        delay_ms(20);
        busy_reset[pass] = gpio_get_level(busy);
        if (err == ESP_OK) err = gpio_set_level(reset, 1);
        /* ST7305 reset recovery can take 120 ms from sleep-out. */
        delay_ms(120);
        busy_ready[pass] = gpio_get_level(busy);
        if (err == ESP_OK) err = read_register(spi, dc, 0x0a, &power[pass]);
        if (err == ESP_OK) err = read_register(spi, dc, 0x2f, &status[pass]);
        ESP_LOGI("cl32-display", "probe pull-%s: RDDPM=%02x status=%02x reset=%d ready=%d",
                 pass == 0 ? "up" : "down", power[pass], status[pass],
                 busy_reset[pass], busy_ready[pass]);
    }
    if (err == ESP_OK) {
        /* ST7305 RDDPM POR=08 (normal, asleep, display off).
         * SSD1685 status POR=01 (chip ID=1, not busy).
         * See ST7305 v0.2 8.1.5 and SSD1685 rev0.11 command 2Fh. */
        const bool rlcd = power[0] == 0x08 && power[1] == 0x08 &&
            status[0] != 0x01 && status[1] != 0x01;
        const bool epd = status[0] == 0x01 && status[1] == 0x01 &&
            power[0] != 0x08 && power[1] != 0x08;
        selected = rlcd == epd ? CL32_DISPLAY_UNKNOWN :
            rlcd ? CL32_DISPLAY_ST7305 : CL32_DISPLAY_SSD1685;
        const bool no_readback = power[0] == 0xff && status[0] == 0xff &&
            ((power[1] == 0xff && status[1] == 0xff) ||
             (power[1] == 0x00 && status[1] == 0x00));
        if (no_readback && busy_ready[0] == 0 && busy_ready[1] == 0) {
            bool reset_response = false;
            err = probe_busy_reset(spi, dc, busy, &reset_response);
            if (err == ESP_OK) {
                if (reset_response) selected = CL32_DISPLAY_SSD1685;
                /* RLCD TE floats during hardware reset, then drives LOW.
                 * Require that transition under both pulls, so disconnected
                 * and grounded control lines are not accepted as an RLCD. */
                else if (busy_reset[0] == 1 && busy_reset[1] == 0)
                    selected = CL32_DISPLAY_ST7305;
                ESP_LOGI("cl32-display", "software reset BUSY response: %s",
                         reset_response ? "yes" : "no");
            }
        }
        if (err == ESP_OK && selected == CL32_DISPLAY_UNKNOWN) err = ESP_ERR_NOT_SUPPORTED;
    }
    if (spi != NULL) {
        const esp_err_t removed = spi_bus_remove_device(spi);
        if (err == ESP_OK) err = removed;
    }
    esp_err_t restored = gpio_set_pull_mode(mosi, GPIO_FLOATING);
    if (err == ESP_OK) err = restored;
    restored = gpio_set_pull_mode(busy, GPIO_FLOATING);
    if (err == ESP_OK) err = restored;
    if (err == ESP_OK) *controller = selected;
    return err;
}
