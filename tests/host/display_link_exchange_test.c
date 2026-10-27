#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ESP_OK 0
#define portMAX_DELAY UINT32_MAX
typedef int esp_err_t;
typedef uint32_t TickType_t;
typedef void *spi_device_handle_t;
typedef struct {
    int clock_speed_hz, mode, spics_io_num, queue_size;
} spi_device_interface_config_t;
typedef struct {
    unsigned length, rxlength;
    const void *tx_buffer;
    void *rx_buffer;
} spi_transaction_t;
typedef struct {
    struct { int cs, ready; } config;
    const char *bus;
    spi_device_handle_t spi;
    uint32_t spi_hz;
    uint64_t last_cs_us;
    bool ready_enabled;
} spi_transport_t;

static uint64_t clock_us, acquire_delay;
static unsigned acquisitions, releases, transfers, selections;
static bool acquired;
static int acquire_result, transfer_result;
static uint64_t now_us(void *ctx) { (void)ctx; return clock_us; }
static void sleep_us(void *ctx, uint32_t us) { (void)ctx; clock_us += us; }
static void esp_rom_delay_us(uint32_t us) { clock_us += us; }
static int gpio_get_level(int pin) { (void)pin; return 0; }
static esp_err_t gpio_set_level(int pin, int value) {
    assert(pin == 12 && acquired);
    if (!value) selections++;
    return ESP_OK;
}
static esp_err_t spi_bus_remove_device(spi_device_handle_t spi) {
    assert(spi && !acquired);
    return ESP_OK;
}
static esp_err_t solar_os_bus_spi_add_device(
        const char *bus, const spi_device_interface_config_t *cfg,
        spi_device_handle_t *spi) {
    assert(!strcmp(bus, "spi0") && cfg->mode == 1);
    assert(cfg->spics_io_num == -1 && cfg->queue_size == 1);
    *spi = (void *)1;
    return ESP_OK;
}
static esp_err_t spi_device_acquire_bus(spi_device_handle_t spi, TickType_t wait) {
    assert(spi && !acquired);
    /* ESP-IDF rejects every finite timeout before touching the bus. */
    assert(wait == portMAX_DELAY);
    acquisitions++;
    clock_us += acquire_delay;
    acquired = acquire_result == ESP_OK;
    return acquire_result;
}
static void spi_device_release_bus(spi_device_handle_t spi) {
    assert(spi && acquired);
    acquired = false;
    releases++;
}
static esp_err_t spi_device_polling_transmit(spi_device_handle_t spi,
                                            spi_transaction_t *tr) {
    assert(spi && acquired && tr->length == 2048 && tr->rxlength == 2048);
    transfers++;
    memcpy(tr->rx_buffer, tr->tx_buffer, 256);
    return transfer_result;
}

#include "display_link_exchange.inc"

int main(void) {
    spi_transport_t d = {.config = {.cs = 12, .ready = -1},
                                 .bus = "spi0"};
    uint8_t tx[256] = {0x53, 0x44, 0x4c, 0x50}, rx[256] = {0};
    clock_us = 1000;
    assert(exchange(&d, tx, rx, 100000, 100, 10000) == 0);
    assert(acquisitions == 1 && releases == 1 && transfers == 1);
    assert(selections == 1 && !acquired && !memcmp(tx, rx, 256));

    /* A delayed bus acquisition must not start an expired cell. */
    acquire_delay = 20000;
    assert(exchange(&d, tx, rx, 100000, 0, 10000) == -1);
    assert(acquisitions == 2 && releases == 2 && transfers == 1);
    assert(selections == 1 && !acquired);

    acquire_delay = 0;
    acquire_result = -1;
    assert(exchange(&d, tx, rx, 100000, 0, 50000) == -1);
    assert(acquisitions == 3 && releases == 2 && !acquired);

    acquire_result = ESP_OK;
    transfer_result = -1;
    assert(exchange(&d, tx, rx, 100000, 0, 50000) == -1);
    assert(releases == 3 && selections == 2 && !acquired);

    /* READY staying low must time out before acquiring or selecting. */
    d.ready_enabled = true;
    uint64_t deadline = clock_us + 1000;
    assert(exchange(&d, tx, rx, 100000, 0, deadline) == -1);
    assert(clock_us >= deadline && acquisitions == 4 && selections == 2);
    puts("SDLP SPI acquisition, transfer errors and deadlines passed");
    return 0;
}
