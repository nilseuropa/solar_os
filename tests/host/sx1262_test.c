#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static StaticSemaphore_t mutex;
    return xSemaphoreCreateMutexStatic(&mutex);
}

#include "../../src/drivers/sx1262.c"

static int64_t now_us;
static int64_t sleep_us;
static bool asleep, stuck_busy, fail_wake;
static bool retained;
static unsigned transfers, wakes;
static uint8_t opcodes[1024];
static uint8_t rx_gain = 0x94;
static bool fail_gain;
static uint8_t gfsk_bw;

int64_t esp_timer_get_time(void) { return now_us; }
void esp_rom_delay_us(uint32_t us) { now_us += us; }
void vTaskDelay(TickType_t ticks) { now_us += (int64_t)ticks * 1000; }
int gpio_get_level(gpio_num_t pin) { assert(pin == 13); return asleep || stuck_busy; }
esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{ (void)pin; (void)level; return ESP_OK; }

esp_err_t solar_os_bus_spi_transfer(const char *name, int cs, uint8_t mode,
    uint32_t hz, const uint8_t *tx, uint8_t *rx, size_t len)
{
    assert(strcmp(name, "spi0") == 0 && cs == 9 && mode == 0 && hz != 0);
    assert(transfers < sizeof(opcodes));
    opcodes[transfers++] = tx[0];
    if (rx != NULL) memset(rx, 0, len);
    if (tx[0] == SX1262_OP_GET_STATUS && (asleep || fail_wake)) {
        wakes++;
        assert(now_us - sleep_us >= 500);
        if (fail_wake) return ESP_FAIL;
        asleep = false;
        /* SDO is undefined during wake; the driver must ignore this byte. */
        if (rx != NULL) memset(rx, 0xff, len);
        return ESP_OK;
    }
    assert(!asleep && !stuck_busy);
    if (tx[0] == SX1262_OP_SET_MODULATION_PARAMS && len == 9) gfsk_bw = tx[5];
    if (tx[0] == SX1262_OP_GET_RSSI_INST) {
        assert(len == 3 && tx[1] == 0 && tx[2] == 0);
        rx[1] = 0x24; /* Status is not RSSI. */
        rx[2] = 201;
    }
    if (tx[0] == SX1262_OP_WRITE_REGISTER && tx[1] == 0x08 && tx[2] == 0xac) {
        assert(len == 4);
        if (fail_gain) return ESP_FAIL;
        rx_gain = tx[3];
    }
    if (tx[0] == SX1262_OP_SET_RX) {
        assert(rx_gain == 0x94 || rx_gain == 0x96);
    }
    if (tx[0] == SX1262_OP_SET_SLEEP) {
        assert(len == 2);
        retained = (tx[1] & 0x04U) != 0U;
        asleep = true;
        rx_gain = 0x94; /* Gain is not in the default warm-sleep retention list. */
        sleep_us = now_us;
    }
    if (tx[0] == SX1262_OP_GET_IRQ_STATUS) {
        assert(len == 4);
        rx[3] = SX1262_IRQ_TX_DONE;
    }
    return ESP_OK;
}

static void sleep_radio(sx1262_t *dev)
{
    assert(sx1262_set_state(dev, SOLAR_OS_RADIO_STATE_SLEEP) == ESP_OK);
    assert(asleep && dev->state == SOLAR_OS_RADIO_STATE_SLEEP);
}

int main(void)
{
    sx1262_t dev = {.spi_bus = "spi0", .cs_pin = 9, .busy_pin = 13,
        .speed_hz = 2000000, .state = SOLAR_OS_RADIO_STATE_STANDBY,
        .config = {.frequency_hz = 868000000, .modulation = SOLAR_OS_RADIO_MODULATION_LORA,
            .preamble_len = 8, .spreading_factor = 7, .rx_bandwidth_hz = 125000,
            .coding_rate_denominator = 5, .sync_word_len = 1, .sync_word = {0x12},
            .tx_power_dbm = 14, .variable_length = true, .payload_length = 255}};
    int16_t dbm = 0;
    assert(sx1262_read_rssi(&dev, &dbm) == ESP_ERR_INVALID_STATE);
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_RX) == ESP_OK);
    dev.has_last_packet = true; dev.last_rssi_dbm = -75;
    assert(sx1262_read_rssi(&dev, &dbm) == ESP_OK && dbm == -100);
    assert(dev.last_rssi_dbm == -75 && dev.state == SOLAR_OS_RADIO_STATE_RX);
    sleep_radio(&dev);
    unsigned no_wake = transfers;
    assert(sx1262_read_rssi(&dev, &dbm) == ESP_ERR_INVALID_STATE && transfers == no_wake);
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_STANDBY) == ESP_OK);
    assert(!asleep && dev.state == SOLAR_OS_RADIO_STATE_STANDBY && wakes == 1);
    assert(retained && dev.config.frequency_hz == 868000000);

    sleep_radio(&dev);
    unsigned before = transfers;
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_SLEEP) == ESP_OK);
    assert(transfers == before && asleep); /* Repeated sleep must not wake. */
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_RX) == ESP_OK);
    assert(!asleep && dev.state == SOLAR_OS_RADIO_STATE_RX && retained);
    assert(opcodes[before] == SX1262_OP_GET_STATUS && opcodes[before + 1] == SX1262_OP_SET_STANDBY);
    assert(opcodes[before + 2] == SX1262_OP_WRITE_REGISTER && opcodes[before + 3] == SX1262_OP_SET_RX);

    sleep_radio(&dev);
    fail_wake = true;
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_STANDBY) == ESP_FAIL);
    assert(dev.state == SOLAR_OS_RADIO_STATE_SLEEP && asleep);
    fail_wake = false;
    stuck_busy = true;
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_STANDBY) == ESP_ERR_TIMEOUT);
    assert(dev.state == SOLAR_OS_RADIO_STATE_SLEEP);
    stuck_busy = false;
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_STANDBY) == ESP_OK);

    /* Packet I/O and configuration also wake through the common command path. */
    sleep_radio(&dev);
    solar_os_radio_packet_t packet = {.len = 1, .data = {0x42}};
    assert(sx1262_send(&dev, &packet, 100) == ESP_OK);
    assert(!asleep && dev.state == SOLAR_OS_RADIO_STATE_STANDBY && retained);
    sleep_radio(&dev);
    solar_os_radio_config_t config = dev.config;
    config.frequency_hz = 915000000;
    assert(sx1262_configure(&dev, &config) == ESP_OK);
    assert(!asleep && dev.state == SOLAR_OS_RADIO_STATE_STANDBY);
    assert(dev.config.frequency_hz == 915000000);
    sleep_radio(&dev);
    assert(sx1262_receive(&dev, &packet, 8) == ESP_ERR_TIMEOUT);
    assert(!asleep && dev.state == SOLAR_OS_RADIO_STATE_RX && retained);
    config = dev.config;
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_BOOSTED;
    assert(sx1262_configure(&dev, &config) == ESP_OK && rx_gain == 0x96);
    sleep_radio(&dev);
    assert(rx_gain == 0x94);
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_RX) == ESP_OK && rx_gain == 0x96);
    sleep_radio(&dev);
    assert(sx1262_receive(&dev, &packet, 0) == ESP_ERR_TIMEOUT && rx_gain == 0x96);
    before = transfers;
    assert(sx1262_receive(&dev, &packet, 0) == ESP_ERR_TIMEOUT);
    assert(transfers == before + 1); /* Polling does not re-arm or rewrite gain. */
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_POWER_SAVING;
    assert(sx1262_configure(&dev, &config) == ESP_OK && rx_gain == 0x94);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_G1;
    before = transfers;
    assert(sx1262_configure(&dev, &config) == ESP_ERR_NOT_SUPPORTED && transfers == before);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_BOOSTED;
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_RX) == ESP_OK);
    fail_gain = true;
    assert(sx1262_configure(&dev, &config) == ESP_FAIL);
    assert(dev.state == SOLAR_OS_RADIO_STATE_STANDBY);
    assert(dev.config.rx_gain == SOLAR_OS_RADIO_RX_GAIN_POWER_SAVING);
    fail_gain = false;
    assert(sx1262_receive(&dev, &packet, 0) == ESP_ERR_TIMEOUT && rx_gain == 0x94);
    assert(sx1262_configure(&dev, &config) == ESP_OK);
    uint32_t settle_ms = 0;
    const uint8_t saved_rx_gain = rx_gain;
    before = transfers;
    assert(sx1262_tune_rx(&dev, config.frequency_hz + 125000, &settle_ms) == ESP_OK);
    assert(settle_ms == 2 && dev.state == SOLAR_OS_RADIO_STATE_RX);
    assert(transfers == before + 4); /* XOSC standby, frequency, IRQ clear, RX. */
    assert(opcodes[before] == SX1262_OP_SET_STANDBY);
    assert(opcodes[before + 1] == SX1262_OP_SET_RF_FREQUENCY);
    assert(opcodes[before + 2] == SX1262_OP_CLEAR_IRQ_STATUS);
    assert(opcodes[before + 3] == SX1262_OP_SET_RX);
    before = transfers;
    assert(sx1262_tune_rx(&dev, 433000000, &settle_ms) == ESP_OK);
    assert(transfers == before + 6 && opcodes[before + 1] == SX1262_OP_CALIBRATE_IMAGE);
    assert(dev.image_cal_min_hz <= 433000000 && dev.image_cal_max_hz >= 433000000);
    assert(sx1262_tune_rx(&dev, 800000000, &settle_ms) == ESP_OK);
    assert(dev.image_cal_min_hz <= 800000000 && dev.image_cal_max_hz >= 800000000);
    assert(dev.config.rx_gain == config.rx_gain && rx_gain == saved_rx_gain);
    assert(sx1262_tune_rx(&dev, config.frequency_hz, &settle_ms) == ESP_OK);
    sleep_radio(&dev);
    fail_gain = true;
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_RX) == ESP_FAIL);
    assert(dev.state == SOLAR_OS_RADIO_STATE_STANDBY);
    assert(dev.config.rx_gain == SOLAR_OS_RADIO_RX_GAIN_BOOSTED);
    fail_gain = false;
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_RX) == ESP_OK);
    assert(rx_gain == 0x96);
    config.modulation = SOLAR_OS_RADIO_MODULATION_GFSK;
    config.rx_bandwidth_hz = 62500;
    assert(sx1262_configure(&dev, &config) == ESP_OK && gfsk_bw == 0x1b);
    assert(dev.config.rx_bandwidth_hz == 78200);
    config.rx_bandwidth_hz = 250000;
    assert(sx1262_configure(&dev, &config) == ESP_OK && gfsk_bw == 0x19);
    assert(dev.config.rx_bandwidth_hz == 312000);
    config.rx_bandwidth_hz = 500000;
    before = transfers;
    assert(sx1262_configure(&dev, &config) == ESP_ERR_INVALID_ARG && transfers == before);
    config.frequency_hz = 1000000000;
    assert(sx1262_configure(&dev, &config) == ESP_ERR_INVALID_ARG && transfers == before);
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_RX) == ESP_OK);
    /* Ordinary BUSY waits still apply when the radio is already awake. */
    stuck_busy = true;
    before = transfers;
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_STANDBY) == ESP_ERR_TIMEOUT);
    assert(transfers == before && dev.state == SOLAR_OS_RADIO_STATE_RX);
    puts("SX1262 sleep/wake, retained configuration, failure retry and packet I/O: ok");
    return 0;
}
