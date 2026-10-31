#include "epd_sticky_identify.h"

/* Protocol reference and license: doc/licenses/seeed-ssd2677-notice.txt. */

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

#define TRY(call) do { esp_err_t err_ = (call); if (err_ != ESP_OK) return err_; } while (0)

static void delay_ms(unsigned ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t reset_panel(int reset)
{
    TRY(gpio_set_level(reset, 0));
    delay_ms(20);
    TRY(gpio_set_level(reset, 1));
    delay_ms(50);
    return ESP_OK;
}

static esp_err_t probe_id(spi_device_handle_t spi, int dc, uint8_t *id)
{
    TRY(spi_device_acquire_bus(spi, portMAX_DELAY));
    esp_err_t err = gpio_set_level(dc, 0);
    spi_transaction_t tx = {.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_CS_KEEP_ACTIVE,
        .length = 8, .tx_data = {0x70}};
    if (err == ESP_OK) err = spi_device_polling_transmit(spi, &tx);
    if (err == ESP_OK) err = gpio_set_level(dc, 1);
    spi_transaction_t rx = {.rxlength = 8, .rx_buffer = id};
    if (err == ESP_OK) err = spi_device_polling_transmit(spi, &rx);
    spi_device_release_bus(spi);
    return err;
}

static esp_err_t busy_ready_level(int busy, int *ready)
{
    /* Match Seeed's BUSY fallback, but require a stable result and reject a
     * floating/disconnected BUSY line. Active LOW=SSD2677, HIGH=SSD1677. */
    int level = gpio_get_level(busy);
    const int64_t deadline = esp_timer_get_time() + 1500000;
    int64_t stable_since = esp_timer_get_time();
    while (esp_timer_get_time() < deadline) {
        delay_ms(2);
        const int sample = gpio_get_level(busy);
        if (sample != level) { level = sample; stable_since = esp_timer_get_time(); }
        if (esp_timer_get_time() - stable_since >= 200000) {
            gpio_config_t input = {.pin_bit_mask = 1ULL << busy, .mode = GPIO_MODE_INPUT,
                .pull_up_en = level ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
                .pull_down_en = level ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE};
            TRY(gpio_config(&input));
            delay_ms(2);
            const bool driven = gpio_get_level(busy) == level;
            input.pull_up_en = GPIO_PULLUP_DISABLE;
            input.pull_down_en = GPIO_PULLDOWN_DISABLE;
            TRY(gpio_config(&input));
            if (!driven) return ESP_ERR_NOT_SUPPORTED;
            *ready = level;
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t epd_sticky_identify(const char *spi_bus, int cs, int dc, int reset,
    int busy, int power, epd_sticky_controller_t *controller)
{
    if (!controller) return ESP_ERR_INVALID_ARG;
    *controller = EPD_STICKY_UNKNOWN;
    if (!spi_bus || !spi_bus[0]) return ESP_ERR_INVALID_ARG;
    const int pins[] = {cs,dc,reset,busy,power};
    for (unsigned i = 0; i < 5; i++) {
        if (i == 4 && pins[i] == -1) continue;
        if (pins[i] < 0 || pins[i] >= GPIO_NUM_MAX ||
            (i == 3 ? !GPIO_IS_VALID_GPIO(pins[i]) : !GPIO_IS_VALID_OUTPUT_GPIO(pins[i])))
            return ESP_ERR_INVALID_ARG;
        for (unsigned j = 0; j < i; j++) if (pins[i] == pins[j]) return ESP_ERR_INVALID_ARG;
    }
    gpio_config_t gpio = {.pin_bit_mask = (1ULL << dc) | (1ULL << reset), .mode = GPIO_MODE_OUTPUT};
    if (power >= 0) gpio.pin_bit_mask |= 1ULL << power;
    TRY(gpio_config(&gpio));
    gpio = (gpio_config_t){.pin_bit_mask = 1ULL << busy, .mode = GPIO_MODE_INPUT};
    TRY(gpio_config(&gpio));
    TRY(gpio_set_level(dc, 1));
    if (power >= 0) TRY(gpio_set_level(power, 1));
    delay_ms(20);
    const spi_device_interface_config_t config = {
        .clock_speed_hz = 500000, .mode = 0, .spics_io_num = cs, .queue_size = 1,
        .flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
    };
    spi_device_handle_t spi = NULL;
    esp_err_t err = solar_os_bus_spi_add_device(spi_bus, &config, &spi);
    int previous = -1;
    for (unsigned pass = 0; err == ESP_OK && pass < 2; pass++) {
        err = reset_panel(reset);
        uint8_t id = 0;
        if (err == ESP_OK) err = probe_id(spi, dc, &id);
        int ready = -1;
        if (err == ESP_OK) err = busy_ready_level(busy, &ready);
        /* A working ID must agree with polarity. Undriven 00/ff readback is
         * common on Sticky and is never taken as evidence for SSD1677. */
        if (err == ESP_OK && ((id == 0x07 && ready != 1) ||
            (pass && ready != previous))) err = ESP_ERR_NOT_SUPPORTED;
        previous = ready;
    }
    if (spi) {
        const esp_err_t removed = spi_bus_remove_device(spi);
        if (err == ESP_OK) err = removed;
    }
    if (err == ESP_OK) *controller = previous ? EPD_STICKY_SSD2677 : EPD_STICKY_SSD1677;
    else if (power >= 0) (void)gpio_set_level(power, 0);
    return err;
}
