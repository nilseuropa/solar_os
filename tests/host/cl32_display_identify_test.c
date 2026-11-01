#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/drivers/cl32_display_identify.c"

static int pull, busy_pull, resets;
static int64_t time_us, reset_time;
static uint8_t command;
static unsigned acquired, released, removed, reset_commands;
static bool floating, inconsistent, ambiguous, grounded, read_high, reset_low, resetting;
static bool stuck_reset, corrupt, ambiguous_first;
static enum { CONNECTED, OPEN, GROUNDED, HIGH } control_line;
static esp_err_t add_error, transfer_error, remove_error, reset_error, restore_error;
static cl32_display_controller_t actual;

int64_t esp_timer_get_time(void) { return time_us; }
void esp_rom_delay_us(uint32_t us) { time_us += us; }
void vTaskDelay(TickType_t ticks) { assert(ticks); time_us += ticks * 1000; }
esp_err_t gpio_config(const gpio_config_t *gpio)
{
    assert((gpio->mode == GPIO_MODE_OUTPUT && gpio->pin_bit_mask == ((1ULL << 12) | (1ULL << 13))) ||
           (gpio->mode == GPIO_MODE_INPUT && gpio->pin_bit_mask == (1ULL << 14)));
    return ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    if (pin == 12) {
        reset_low = !level;
        if (level) resets++;
        else resetting = false;
    }
    return ESP_OK;
}
int gpio_get_level(gpio_num_t pin)
{
    assert(pin == 14);
    if (control_line == GROUNDED) return 0;
    if (control_line == HIGH) return 1;
    if (control_line == OPEN || reset_low) return busy_pull == GPIO_PULLUP_ONLY;
    return resetting && actual == CL32_DISPLAY_SSD1685 &&
        (stuck_reset || time_us - reset_time < 5000);
}
esp_err_t gpio_set_pull_mode(gpio_num_t pin, int mode)
{
    assert(pin == 10 || pin == 14);
    if (pin == 10) pull = mode;
    else busy_pull = mode;
    return pin == 14 && mode == GPIO_FLOATING ? restore_error : ESP_OK;
}
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
        command = tx->tx_data[0];
        if (command == 0x12) {
            assert(!(tx->flags & SPI_TRANS_CS_KEEP_ACTIVE));
            reset_commands++;
            resetting = true; reset_time = time_us;
            return reset_error;
        }
        assert(tx->flags & SPI_TRANS_CS_KEEP_ACTIVE);
        assert(command == 0x0a || command == 0x2f);
        return ESP_OK;
    }
    uint8_t value = pull == GPIO_PULLUP_ONLY ? 0xff : 0;
    cl32_display_controller_t selected = actual;
    if (inconsistent && resets == 2) selected = CL32_DISPLAY_ST7305;
    const bool both = ambiguous || (ambiguous_first && resets == 1);
    if (!floating && !grounded) {
        if (command == 0x0a && (selected == CL32_DISPLAY_ST7305 || both)) value = 0x08;
        if (command == 0x2f && (selected == CL32_DISPLAY_SSD1685 || both)) value = 0x01;
    }
    if (grounded) value = 0;
    if (read_high) value = 0xff;
    if (corrupt) value = 0x55;
    *(uint8_t *)tx->rx_buffer = value;
    return ESP_OK;
}

static esp_err_t identify(cl32_display_controller_t *result)
{
    acquired = released = removed = resets = reset_commands = 0;
    const esp_err_t err = cl32_display_identify("spi0", 6, 13, 12, 10, 14, result);
    assert(acquired == released);
    assert(removed == (add_error == ESP_OK ? 1U : 0U));
    assert(pull == GPIO_FLOATING && busy_pull == GPIO_FLOATING);
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
    /* Measured RLCD: ff readback under both pulls, TE floats in reset and
     * drives LOW afterward. The same fallback must also handle floating SDA. */
    actual = CL32_DISPLAY_ST7305; read_high = true;
    assert(identify(&result) == ESP_OK && result == actual && reset_commands == 1);
    read_high = false;
    floating = true;
    assert(identify(&result) == ESP_OK && result == actual && reset_commands == 1);
    actual = CL32_DISPLAY_SSD1685;
    assert(identify(&result) == ESP_OK && result == actual && reset_commands == 1);
    read_high = true;
    assert(identify(&result) == ESP_OK && result == actual && reset_commands == 1);
    stuck_reset = true;
    assert(identify(&result) == ESP_ERR_TIMEOUT);
    stuck_reset = false; reset_error = ESP_FAIL;
    assert(identify(&result) == ESP_FAIL);
    reset_error = ESP_OK;
    actual = CL32_DISPLAY_ST7305;
    control_line = OPEN;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED && reset_commands == 0);
    control_line = GROUNDED;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED);
    control_line = HIGH;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED && reset_commands == 0);
    control_line = CONNECTED; read_high = false;
    actual = CL32_DISPLAY_UNKNOWN; control_line = OPEN;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED);
    control_line = CONNECTED; floating = false; grounded = true;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED);
    grounded = false; ambiguous = true;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED);
    ambiguous = false; ambiguous_first = true;
    actual = CL32_DISPLAY_ST7305;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED);
    actual = CL32_DISPLAY_SSD1685;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED);
    ambiguous_first = false; inconsistent = true;
    actual = CL32_DISPLAY_SSD1685;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED);
    inconsistent = false; corrupt = true;
    assert(identify(&result) == ESP_ERR_NOT_SUPPORTED);
    corrupt = false; transfer_error = ESP_FAIL;
    assert(identify(&result) == ESP_FAIL);
    transfer_error = ESP_OK; remove_error = ESP_FAIL;
    assert(identify(&result) == ESP_FAIL);
    remove_error = ESP_OK; restore_error = ESP_FAIL;
    assert(identify(&result) == ESP_FAIL);
    restore_error = ESP_OK; add_error = ESP_ERR_NO_MEM;
    assert(identify(&result) == ESP_ERR_NO_MEM);
    assert(cl32_display_identify("spi0", 6, 13, 13, 10, 14, &result) == ESP_ERR_INVALID_ARG);
    assert(cl32_display_identify("spi0", 6, 13, 12, 10, 13, &result) == ESP_ERR_INVALID_ARG);
    assert(cl32_display_identify("spi0", 6, 13, 12, 10, -1, &result) == ESP_ERR_INVALID_ARG);
    assert(cl32_display_identify("spi0", 6, 13, 12, 10, GPIO_NUM_MAX, &result) == ESP_ERR_INVALID_ARG);
    assert(cl32_display_identify("spi0", 6, 13, 12, 10, 14, NULL) == ESP_ERR_INVALID_ARG);
    puts("CL-32 controller identification tests passed");
}
