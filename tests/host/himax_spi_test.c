#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
typedef struct { size_t length; const void *tx_buffer; void *rx_buffer; } spi_transaction_t;
typedef struct { int unused; } solar_os_gpio_line_ref_t;
typedef struct {
    void *spi;
    uint8_t packet[256];
    bool have_ready;
    solar_os_gpio_line_ref_t ready_line;
    size_t transfer_max;
} himax_device_t;
static unsigned calls, acquired, released, fail_at;
static bool read_mode, ready = true;
static esp_err_t ready_error, acquire_error;
static uint16_t available = 600;
static const uint8_t *expected_write;
static size_t expected_size, requested;
static void wait_ms(void *ctx, uint32_t ms) { assert(ctx && ms == 2); }
static esp_err_t solar_os_gpio_line_read(const solar_os_gpio_line_ref_t *line, bool *level)
{ assert(line); *level = ready; return ready_error; }
static esp_err_t spi_device_acquire_bus(void *spi, unsigned timeout)
{ assert(spi && timeout == portMAX_DELAY); acquired++; return acquire_error; }
static void spi_device_release_bus(void *spi) { assert(spi); released++; }
static esp_err_t spi_device_polling_transmit(void *spi, spi_transaction_t *transaction)
{
    assert(spi && transaction->length % 8 == 0);
    const unsigned index = calls++;
    if (fail_at == calls) return ESP_FAIL;
    const uint8_t *tx = transaction->tx_buffer;
    size_t length = transaction->length / 8;
    if (!read_mode) {
        const size_t payload = expected_size > 250 ? 250 : expected_size;
        assert(length == 256 && tx && !transaction->rx_buffer);
        assert(tx[0] == 0x10 && tx[1] == 2 && tx[2] == 0 && tx[3] == payload);
        assert(!memcmp(tx + 4, expected_write, payload) && tx[4 + payload] == 0xff && tx[5 + payload] == 0xff);
        for (size_t i = 6 + payload; i < 256; i++) assert(!tx[i]);
        expected_write += payload; expected_size -= payload;
    } else if (index == 0) {
        assert(length == 256 && tx[0] == 0x10 && tx[1] == 3 && !tx[2] && !tx[3]);
        assert(tx[4] == 0xff && tx[5] == 0xff);
    } else if (index == 1) {
        assert(length == 2 && !tx && transaction->rx_buffer);
        uint8_t *rx = transaction->rx_buffer; rx[0] = available >> 8; rx[1] = available;
    } else if (index == 2) {
        assert(length == 256 && tx[0] == 0x10 && tx[1] == 1);
        requested = ((size_t)tx[2] << 8) | tx[3];
        assert(tx[4] == 0xff && tx[5] == 0xff);
    } else {
        assert(index == 3 && !tx && length == requested);
        memset(transaction->rx_buffer, 0x5a, length);
    }
    return ESP_OK;
}
/* PRODUCTION_TRANSPORT */
int main(void)
{
    himax_device_t d = {.spi = &d, .have_ready = true, .transfer_max = 4096};
    uint8_t outgoing[253]; memset(outgoing, 0xa5, sizeof(outgoing));
    expected_write = outgoing; expected_size = sizeof(outgoing);
    assert(write_bytes(&d, outgoing, sizeof(outgoing)) == ESP_OK && !expected_size);
    assert(calls == 2 && acquired == 1 && released == 1);
    uint8_t incoming[1024]; size_t length;
    read_mode = true; calls = acquired = released = 0;
    assert(read_bytes(&d, incoming, sizeof(incoming), &length) == ESP_OK && length == 600);
    assert(incoming[599] == 0x5a && calls == 4 && acquired == released);
    d.transfer_max = 256; calls = 0;
    assert(read_bytes(&d, incoming, sizeof(incoming), &length) == ESP_OK && length == 256);
    for (unsigned failure = 1; failure <= 4; failure++) {
        calls = acquired = released = 0; fail_at = failure;
        assert(read_bytes(&d, incoming, sizeof(incoming), &length) == ESP_FAIL && !length);
        assert(acquired == 1 && released == 1);
    }
    fail_at = 0; available = 0xffff; calls = 0;
    assert(read_bytes(&d, incoming, sizeof(incoming), &length) == ESP_OK && !length && calls == 2);
    ready = false; calls = acquired = released = 0;
    assert(read_bytes(&d, incoming, sizeof(incoming), &length) == ESP_OK && !length && !calls && !acquired);
    ready_error = ESP_FAIL;
    assert(read_bytes(&d, incoming, sizeof(incoming), &length) == ESP_FAIL && !acquired);
    ready_error = ESP_OK; ready = true; acquire_error = ESP_ERR_TIMEOUT;
    assert(read_bytes(&d, incoming, sizeof(incoming), &length) == ESP_ERR_TIMEOUT && !released);
}
