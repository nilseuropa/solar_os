#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../../src/drivers/epd_sticky_identify.c"

static int64_t now_us, reset_time;
static int ready_level, pull_level = -1, pass;
static bool floating, unstable, disagree, transport_fail;
static uint8_t chip_id;
static unsigned acquired, released, removed;

void vTaskDelay(TickType_t ticks)
{ assert(ticks); now_us += (int64_t)ticks * SOLAR_OS_EPD_TEST_TICK_MS * 1000; }
int64_t esp_timer_get_time(void) { return now_us; }
esp_err_t gpio_config(const gpio_config_t *gpio)
{
    if (gpio->mode == GPIO_MODE_INPUT) pull_level = gpio->pull_up_en ? 1 : gpio->pull_down_en ? 0 : -1;
    return ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    if (pin == 17 && level) { reset_time = now_us; pass++; }
    return ESP_OK;
}
int gpio_get_level(gpio_num_t pin)
{
    assert(pin == 18);
    if (floating && pull_level >= 0) return pull_level;
    if (unstable) return (now_us / 10000) & 1;
    const int level = disagree && pass == 2 ? !ready_level : ready_level;
    return now_us - reset_time < 100000 ? !level : level;
}
esp_err_t solar_os_bus_spi_add_device(const char *bus, const spi_device_interface_config_t *config, spi_device_handle_t *spi)
{
    assert(!strcmp(bus,"spi0") && config->spics_io_num == 15 && config->clock_speed_hz == 500000);
    assert(config->flags & SPI_DEVICE_3WIRE);
    *spi = (void *)1; return ESP_OK;
}
esp_err_t spi_device_acquire_bus(spi_device_handle_t spi, unsigned wait)
{ assert(spi && wait); acquired++; return ESP_OK; }
void spi_device_release_bus(spi_device_handle_t spi) { assert(spi); released++; }
esp_err_t spi_bus_remove_device(spi_device_handle_t spi) { assert(spi); removed++; return ESP_OK; }
esp_err_t spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *tx)
{
    assert(spi);
    if (transport_fail) return ESP_FAIL;
    if (tx->rxlength) *(uint8_t *)tx->rx_buffer = chip_id;
    else assert(tx->tx_data[0] == 0x70 && (tx->flags & SPI_TRANS_CS_KEEP_ACTIVE));
    return ESP_OK;
}

static esp_err_t identify(epd_sticky_controller_t *result)
{
    pass = 0; acquired = released = removed = 0;
    const esp_err_t err = epd_sticky_identify("spi0",15,16,17,18,47,result);
    assert(acquired == released && removed == 1);
    if (err != ESP_OK) assert(*result == EPD_STICKY_UNKNOWN);
    return err;
}

int main(void)
{
    epd_sticky_controller_t result;
    for (unsigned id = 0; id < 3; id++) for (int level = 0; level <= 1; level++) {
        chip_id = id == 2 ? 7 : id == 1 ? 255 : 0;
        ready_level = level;
        const esp_err_t expected = chip_id == 7 && !level ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
        assert(identify(&result) == expected);
        if (expected == ESP_OK) assert(result == (level ? EPD_STICKY_SSD2677 : EPD_STICKY_SSD1677));
    }
    chip_id = 0; floating = true;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED);
    floating = false; unstable = true;
    assert(identify(&result) == ESP_ERR_TIMEOUT);
    unstable = false; disagree = true;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED);
    disagree = false; transport_fail = true;
    assert(identify(&result) == ESP_FAIL);
    assert(epd_sticky_identify("spi0",15,16,17,17,47,&result) == ESP_ERR_INVALID_ARG);
    puts("epd_sticky_identify_test: PASS");
    return 0;
}
