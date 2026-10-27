#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static StaticSemaphore_t mutex;
    return xSemaphoreCreateMutexStatic(&mutex);
}

#ifdef TEST_RFM69
#include "../../src/drivers/rfm69.c"
#else
#include "../../src/drivers/rfm95.c"
#endif

static uint8_t registers[128];
static unsigned transfers;
static bool fail_gain;
static int64_t now_us;
int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(TickType_t ticks) { now_us += ticks * 1000; }

esp_err_t solar_os_bus_spi_transfer(const char *name, int cs, uint8_t mode,
    uint32_t hz, const uint8_t *tx, uint8_t *rx, size_t len)
{
    (void)name; (void)cs; (void)mode; (void)hz;
    transfers++;
    assert(len >= 2);
    uint8_t reg = tx[0] & 0x7f;
    if (rx) memset(rx, 0, len);
    if (tx[0] & 0x80) {
        if (reg == REG_LNA && fail_gain) return ESP_FAIL;
        for (size_t i = 1; i < len; i++) registers[(reg + i - 1) & 0x7f] = tx[i];
    } else if (rx) {
        for (size_t i = 1; i < len; i++) rx[i] = registers[(reg + i - 1) & 0x7f];
    }
    return ESP_OK;
}

int main(void)
{
#ifdef TEST_RFM69
    rfm69_t dev = {.variant = RFM69_VARIANT_STANDARD};
    registers[REG_IRQ_FLAGS1] = IRQ1_MODE_READY;
    solar_os_radio_config_t config = {.frequency_hz = 433000000,
        .modulation = SOLAR_OS_RADIO_MODULATION_GFSK, .bitrate_bps = 4800,
        .deviation_hz = 5000, .rx_bandwidth_hz = 10500, .tx_power_dbm = 13,
        .payload_length = 64, .variable_length = true, .preamble_len = 3};
    assert(rfm69_configure(&dev, &config) == ESP_OK);
    assert(dev.config.rx_gain == SOLAR_OS_RADIO_RX_GAIN_AUTO && registers[REG_LNA] == 0x88);
    for (unsigned gain = 1; gain <= 6; gain++) {
        config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_G1 + gain - 1;
        assert(rfm69_configure(&dev, &config) == ESP_OK);
        assert(registers[REG_LNA] == (0x88 | gain));
        assert(dev.config.rx_gain == config.rx_gain);
    }
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_AUTO;
    assert(rfm69_configure(&dev, &config) == ESP_OK && registers[REG_LNA] == 0x88);
    unsigned before = transfers;
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_BOOSTED;
    assert(rfm69_configure(&dev, &config) == ESP_ERR_NOT_SUPPORTED && transfers == before);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_G1;
    fail_gain = true;
    assert(rfm69_configure(&dev, &config) == ESP_FAIL);
    assert(dev.config.rx_gain == SOLAR_OS_RADIO_RX_GAIN_AUTO);
#else
    rfm95_t dev = {0};
    solar_os_radio_config_t config = {.frequency_hz = 868000000,
        .modulation = SOLAR_OS_RADIO_MODULATION_LORA, .tx_power_dbm = 13,
        .spreading_factor = 7, .coding_rate_denominator = 5,
        .rx_bandwidth_hz = 125000, .preamble_len = 8, .sync_word_len = 1,
        .sync_word = {0x12}, .payload_length = 255, .variable_length = true};
    assert(rfm95_configure(&dev, &config) == ESP_OK);
    assert(dev.config.rx_gain == SOLAR_OS_RADIO_RX_GAIN_BOOSTED && registers[REG_LNA] == 0x23);
    assert(registers[REG_MODEM_CONFIG3] & 0x04); /* AGC remains enabled. */
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_POWER_SAVING;
    assert(rfm95_configure(&dev, &config) == ESP_OK && registers[REG_LNA] == 0x20);
    assert(registers[REG_MODEM_CONFIG3] & 0x04);
    config.modulation = SOLAR_OS_RADIO_MODULATION_GFSK;
    registers[REG_FSK_IRQ_FLAGS1] = FSK_IRQ1_MODE_READY;
    assert(rfm95_configure(&dev, &config) == ESP_OK && registers[REG_LNA] == 0x20);
    assert(registers[REG_FSK_RX_CONFIG] & 0x08);
    config = dev.config;
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_BOOSTED;
    assert(rfm95_configure(&dev, &config) == ESP_OK && registers[REG_LNA] == 0x23);
    unsigned before = transfers;
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_G1;
    assert(rfm95_configure(&dev, &config) == ESP_ERR_NOT_SUPPORTED && transfers == before);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_POWER_SAVING;
    fail_gain = true;
    assert(rfm95_configure(&dev, &config) == ESP_FAIL);
    assert(dev.config.rx_gain == SOLAR_OS_RADIO_RX_GAIN_BOOSTED);
#endif
    fail_gain = false;
    config = dev.config;
#ifdef TEST_RFM69
    assert(rfm69_configure(&dev, &config) == ESP_OK);
#else
    assert(rfm95_configure(&dev, &config) == ESP_OK);
#endif
    /* Retuning must preserve all modem/filter/gain registers and packet data. */
    uint8_t old_registers[sizeof(registers)];
    memcpy(old_registers, registers, sizeof(registers));
    uint32_t settle_ms = 0;
    uint32_t frequency = dev.config.frequency_hz + 125000U;
#ifdef TEST_RFM69
    assert(rfm69_tune_rx(&dev, frequency, &settle_ms) == ESP_OK);
#else
    assert(rfm95_tune_rx(&dev, frequency, &settle_ms) == ESP_OK);
#endif
    assert(dev.config.frequency_hz == frequency && dev.state == SOLAR_OS_RADIO_STATE_RX);
    assert(settle_ms >= 2 && settle_ms <= 4);
    for (size_t i = 0; i < sizeof(registers); i++) {
        if (i == REG_OP_MODE || (i >= REG_FRF_MSB && i <= REG_FRF_LSB)) continue;
        assert(registers[i] == old_registers[i]);
    }
    puts("radio RX gain register writes, defaults, AGC and failures: ok");
    return 0;
}
