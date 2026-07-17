#include "rfm69.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "solar_os_spi.h"

#define RFM69_FXOSC_HZ 32000000ULL
#define RFM69_FSTEP_DEN 524288ULL

#define REG_FIFO 0x00
#define REG_OP_MODE 0x01
#define REG_DATA_MODUL 0x02
#define REG_BITRATE_MSB 0x03
#define REG_BITRATE_LSB 0x04
#define REG_FDEV_MSB 0x05
#define REG_FDEV_LSB 0x06
#define REG_FRF_MSB 0x07
#define REG_FRF_MID 0x08
#define REG_FRF_LSB 0x09
#define REG_VERSION 0x10
#define REG_PA_LEVEL 0x11
#define REG_LNA 0x18
#define REG_RX_BW 0x19
#define REG_AFC_BW 0x1A
#define REG_RSSI_VALUE 0x24
#define REG_DIO_MAPPING1 0x25
#define REG_DIO_MAPPING2 0x26
#define REG_IRQ_FLAGS1 0x27
#define REG_IRQ_FLAGS2 0x28
#define REG_PREAMBLE_MSB 0x2C
#define REG_PREAMBLE_LSB 0x2D
#define REG_SYNC_CONFIG 0x2E
#define REG_SYNC_VALUE1 0x2F
#define REG_PACKET_CONFIG1 0x37
#define REG_PAYLOAD_LENGTH 0x38
#define REG_NODE_ADRS 0x39
#define REG_BROADCAST_ADRS 0x3A
#define REG_FIFO_THRESH 0x3C
#define REG_PACKET_CONFIG2 0x3D
#define REG_TEST_DAGC 0x6F

#define OP_MODE_SLEEP 0x00
#define OP_MODE_STANDBY 0x04
#define OP_MODE_TX 0x0C
#define OP_MODE_RX 0x10

#define IRQ1_MODE_READY 0x80
#define IRQ2_FIFO_OVERRUN 0x10
#define IRQ2_PACKET_SENT 0x08
#define IRQ2_PAYLOAD_READY 0x04
#define IRQ2_CRC_OK 0x02

#define PACKET_CONFIG1_VARIABLE 0x80
#define PACKET_CONFIG1_CRC_ON 0x10
#define PACKET_CONFIG1_ADDRESS_NODE 0x02

#define FIFO_WRITE_BIT 0x80
#define RFM69_MODE_WAIT_MS 100

static esp_err_t rfm69_lock(rfm69_t *dev)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev->mutex == NULL) {
        dev->mutex = xSemaphoreCreateMutex();
        if (dev->mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(dev->mutex, portMAX_DELAY);
    return ESP_OK;
}

static void rfm69_unlock(rfm69_t *dev)
{
    if (dev != NULL && dev->mutex != NULL) {
        xSemaphoreGive(dev->mutex);
    }
}

static esp_err_t rfm69_transfer(rfm69_t *dev,
                                const uint8_t *tx,
                                uint8_t *rx,
                                size_t len)
{
    return solar_os_spi_transfer(dev->cs_pin, 0, dev->speed_hz, tx, rx, len);
}

static esp_err_t rfm69_read_reg_locked(rfm69_t *dev, uint8_t reg, uint8_t *value)
{
    uint8_t tx[2] = {reg & 0x7F, 0x00};
    uint8_t rx[2] = {0};

    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ret = rfm69_transfer(dev, tx, rx, sizeof(tx));
    if (ret != ESP_OK) {
        return ret;
    }
    *value = rx[1];
    return ESP_OK;
}

static esp_err_t rfm69_write_reg_locked(rfm69_t *dev, uint8_t reg, uint8_t value)
{
    const uint8_t tx[2] = {(uint8_t)(reg | FIFO_WRITE_BIT), value};
    return rfm69_transfer(dev, tx, NULL, sizeof(tx));
}

static esp_err_t rfm69_write_burst_locked(rfm69_t *dev,
                                          uint8_t reg,
                                          const uint8_t *data,
                                          size_t len)
{
    uint8_t tx[RFM69_MAX_PACKET_LEN + 3];

    if (data == NULL || len == 0 || len > RFM69_MAX_PACKET_LEN + 2) {
        return ESP_ERR_INVALID_ARG;
    }
    tx[0] = reg | FIFO_WRITE_BIT;
    memcpy(&tx[1], data, len);
    return rfm69_transfer(dev, tx, NULL, len + 1);
}

static esp_err_t rfm69_read_burst_locked(rfm69_t *dev, uint8_t reg, uint8_t *data, size_t len)
{
    uint8_t tx[RFM69_MAX_PACKET_LEN + 3];
    uint8_t rx[RFM69_MAX_PACKET_LEN + 3];

    if (data == NULL || len == 0 || len > RFM69_MAX_PACKET_LEN + 2) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(tx, 0, len + 1);
    tx[0] = reg & 0x7F;
    const esp_err_t ret = rfm69_transfer(dev, tx, rx, len + 1);
    if (ret != ESP_OK) {
        return ret;
    }
    memcpy(data, &rx[1], len);
    return ESP_OK;
}

static uint16_t rfm69_bitrate_reg(uint32_t bitrate_bps)
{
    if (bitrate_bps == 0) {
        bitrate_bps = 4800;
    }
    uint64_t value = (RFM69_FXOSC_HZ + bitrate_bps / 2U) / bitrate_bps;
    if (value == 0) {
        value = 1;
    }
    if (value > 0xFFFF) {
        value = 0xFFFF;
    }
    return (uint16_t)value;
}

static uint16_t rfm69_fdev_reg(uint32_t deviation_hz)
{
    if (deviation_hz == 0) {
        deviation_hz = 5000;
    }
    uint64_t value = (((uint64_t)deviation_hz * RFM69_FSTEP_DEN) + RFM69_FXOSC_HZ / 2U) / RFM69_FXOSC_HZ;
    if (value > 0x3FFF) {
        value = 0x3FFF;
    }
    return (uint16_t)value;
}

static uint32_t rfm69_frf_reg(uint32_t frequency_hz)
{
    return (uint32_t)((((uint64_t)frequency_hz * RFM69_FSTEP_DEN) + RFM69_FXOSC_HZ / 2U) / RFM69_FXOSC_HZ);
}

static uint8_t rfm69_data_modul_value(solar_os_radio_modulation_t modulation)
{
    switch (modulation) {
    case SOLAR_OS_RADIO_MODULATION_GFSK:
    case SOLAR_OS_RADIO_MODULATION_GMSK:
        return 0x01;
    case SOLAR_OS_RADIO_MODULATION_OOK:
        return 0x08;
    case SOLAR_OS_RADIO_MODULATION_FSK:
    case SOLAR_OS_RADIO_MODULATION_MSK:
    default:
        return 0x00;
    }
}

static uint8_t rfm69_rx_bw_value(uint32_t bandwidth_hz, bool ook)
{
    if (bandwidth_hz == 0) {
        return 0x55;
    }

    static const uint8_t mant_values[] = {16, 20, 24};
    uint32_t best_hz = UINT32_MAX;
    uint8_t best = 0x55;
    uint32_t max_hz = 0;
    uint8_t max_value = 0x40;

    for (uint8_t mant_code = 0; mant_code < sizeof(mant_values) / sizeof(mant_values[0]); mant_code++) {
        for (uint8_t exp = 0; exp <= 7; exp++) {
            const uint8_t shift = exp + (ook ? 3 : 2);
            const uint32_t actual = (uint32_t)(RFM69_FXOSC_HZ / ((uint64_t)mant_values[mant_code] << shift));
            const uint8_t value = (uint8_t)(0x40 | (mant_code << 3) | exp);
            if (actual > max_hz) {
                max_hz = actual;
                max_value = value;
            }
            if (actual >= bandwidth_hz && actual < best_hz) {
                best_hz = actual;
                best = value;
            }
        }
    }

    if (best_hz == UINT32_MAX) {
        best = max_value;
    }
    return best;
}

static uint8_t rfm69_opmode_for_state(solar_os_radio_state_t state)
{
    switch (state) {
    case SOLAR_OS_RADIO_STATE_SLEEP:
        return OP_MODE_SLEEP;
    case SOLAR_OS_RADIO_STATE_RX:
        return OP_MODE_RX;
    case SOLAR_OS_RADIO_STATE_TX:
        return OP_MODE_TX;
    case SOLAR_OS_RADIO_STATE_STANDBY:
    case SOLAR_OS_RADIO_STATE_UNKNOWN:
    default:
        return OP_MODE_STANDBY;
    }
}

static solar_os_radio_state_t rfm69_state_from_opmode(uint8_t opmode)
{
    switch (opmode & 0x1C) {
    case OP_MODE_SLEEP:
        return SOLAR_OS_RADIO_STATE_SLEEP;
    case OP_MODE_TX:
        return SOLAR_OS_RADIO_STATE_TX;
    case OP_MODE_RX:
        return SOLAR_OS_RADIO_STATE_RX;
    case OP_MODE_STANDBY:
    default:
        return SOLAR_OS_RADIO_STATE_STANDBY;
    }
}

static esp_err_t rfm69_wait_flag_locked(rfm69_t *dev,
                                        uint8_t reg,
                                        uint8_t mask,
                                        uint32_t timeout_ms)
{
    const int64_t start_us = esp_timer_get_time();
    const int64_t timeout_us = (int64_t)timeout_ms * 1000;

    while (true) {
        uint8_t value = 0;
        const esp_err_t ret = rfm69_read_reg_locked(dev, reg, &value);
        if (ret != ESP_OK) {
            return ret;
        }
        if ((value & mask) == mask) {
            return ESP_OK;
        }
        if (timeout_ms == 0 || esp_timer_get_time() - start_us >= timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static esp_err_t rfm69_set_state_locked(rfm69_t *dev, solar_os_radio_state_t state)
{
    const uint8_t opmode = rfm69_opmode_for_state(state);
    esp_err_t ret = rfm69_write_reg_locked(dev, REG_OP_MODE, opmode);
    if (ret != ESP_OK) {
        return ret;
    }
    if (state != SOLAR_OS_RADIO_STATE_SLEEP) {
        ret = rfm69_wait_flag_locked(dev, REG_IRQ_FLAGS1, IRQ1_MODE_READY, RFM69_MODE_WAIT_MS);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    dev->state = state;
    return ESP_OK;
}

static bool rfm69_config_valid(const solar_os_radio_config_t *config)
{
    if (config == NULL ||
        config->frequency_hz < 290000000U ||
        config->frequency_hz > 1020000000U ||
        config->bitrate_bps == 0 ||
        config->bitrate_bps > 300000U ||
        config->sync_word_len > SOLAR_OS_RADIO_SYNC_WORD_MAX ||
        config->payload_length == 0 ||
        config->payload_length > RFM69_MAX_PACKET_LEN ||
        config->tx_power_dbm < -18 ||
        config->tx_power_dbm > 13) {
        return false;
    }

    switch (config->modulation) {
    case SOLAR_OS_RADIO_MODULATION_FSK:
    case SOLAR_OS_RADIO_MODULATION_GFSK:
    case SOLAR_OS_RADIO_MODULATION_MSK:
    case SOLAR_OS_RADIO_MODULATION_GMSK:
    case SOLAR_OS_RADIO_MODULATION_OOK:
        return true;
    case SOLAR_OS_RADIO_MODULATION_NONE:
    case SOLAR_OS_RADIO_MODULATION_LORA:
    default:
        return false;
    }
}

esp_err_t rfm69_init(rfm69_t *dev, int cs_pin, uint32_t speed_hz)
{
    if (dev == NULL || cs_pin < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (dev->mutex == NULL) {
        dev->mutex = xSemaphoreCreateMutex();
        if (dev->mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    dev->cs_pin = cs_pin;
    dev->speed_hz = speed_hz != 0 ? speed_hz : SOLAR_OS_SPI_DEFAULT_SPEED_HZ;
    dev->state = SOLAR_OS_RADIO_STATE_UNKNOWN;
    return ESP_OK;
}

esp_err_t rfm69_probe(rfm69_t *dev, uint8_t *version)
{
    esp_err_t ret = rfm69_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t value = 0;
    ret = rfm69_read_reg_locked(dev, REG_VERSION, &value);
    rfm69_unlock(dev);
    if (ret != ESP_OK) {
        return ret;
    }
    if (version != NULL) {
        *version = value;
    }
    return value == RFM69_VERSION ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t rfm69_configure(rfm69_t *dev, const solar_os_radio_config_t *config)
{
    if (!rfm69_config_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = rfm69_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint16_t bitrate = rfm69_bitrate_reg(config->bitrate_bps);
    const uint16_t fdev = rfm69_fdev_reg(config->deviation_hz);
    const uint32_t frf = rfm69_frf_reg(config->frequency_hz);
    const bool ook = config->modulation == SOLAR_OS_RADIO_MODULATION_OOK;
    const uint8_t payload_length = (uint8_t)(config->payload_length +
                                             (config->has_node_id ? 1U : 0U));
    uint8_t packet_config1 = config->variable_length ? PACKET_CONFIG1_VARIABLE : 0x00;
    if (config->crc_enabled) {
        packet_config1 |= PACKET_CONFIG1_CRC_ON;
    }
    if (config->has_node_id) {
        packet_config1 |= PACKET_CONFIG1_ADDRESS_NODE;
    }

    ret = rfm69_set_state_locked(dev, SOLAR_OS_RADIO_STATE_STANDBY);
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_DATA_MODUL, rfm69_data_modul_value(config->modulation));
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_BITRATE_MSB, (uint8_t)(bitrate >> 8));
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_BITRATE_LSB, (uint8_t)bitrate);
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_FDEV_MSB, (uint8_t)(fdev >> 8));
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_FDEV_LSB, (uint8_t)fdev);
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_FRF_MSB, (uint8_t)(frf >> 16));
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_FRF_MID, (uint8_t)(frf >> 8));
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_FRF_LSB, (uint8_t)frf);
    }
    if (ret == ESP_OK) {
        const uint8_t power = (uint8_t)(config->tx_power_dbm + 18);
        ret = rfm69_write_reg_locked(dev, REG_PA_LEVEL, (uint8_t)(0x80 | (power & 0x1F)));
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_LNA, 0x88);
    }
    if (ret == ESP_OK) {
        const uint8_t bw = rfm69_rx_bw_value(config->rx_bandwidth_hz, ook);
        ret = rfm69_write_reg_locked(dev, REG_RX_BW, bw);
        if (ret == ESP_OK) {
            ret = rfm69_write_reg_locked(dev, REG_AFC_BW, bw);
        }
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_DIO_MAPPING1, 0x00);
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_DIO_MAPPING2, 0x07);
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_PREAMBLE_MSB, (uint8_t)(config->preamble_len >> 8));
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_PREAMBLE_LSB, (uint8_t)config->preamble_len);
    }
    if (ret == ESP_OK) {
        const uint8_t sync_config = config->sync_word_len == 0 ?
            0x00 :
            (uint8_t)(0x80 | ((config->sync_word_len - 1U) << 3));
        ret = rfm69_write_reg_locked(dev, REG_SYNC_CONFIG, sync_config);
    }
    for (uint8_t i = 0; ret == ESP_OK && i < config->sync_word_len; i++) {
        ret = rfm69_write_reg_locked(dev, (uint8_t)(REG_SYNC_VALUE1 + i), config->sync_word[i]);
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_PACKET_CONFIG1, packet_config1);
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_PAYLOAD_LENGTH, payload_length);
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_NODE_ADRS, config->has_node_id ? config->node_id : 0x00);
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_BROADCAST_ADRS, 0xFF);
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_FIFO_THRESH, 0x8F);
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_PACKET_CONFIG2, 0x02);
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_TEST_DAGC, 0x30);
    }
    if (ret == ESP_OK) {
        dev->config = *config;
        dev->state = SOLAR_OS_RADIO_STATE_STANDBY;
    }

    rfm69_unlock(dev);
    return ret;
}

esp_err_t rfm69_set_state(rfm69_t *dev, solar_os_radio_state_t state)
{
    if (state == SOLAR_OS_RADIO_STATE_UNKNOWN || state > SOLAR_OS_RADIO_STATE_TX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = rfm69_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = rfm69_set_state_locked(dev, state);
    rfm69_unlock(dev);
    return ret;
}

esp_err_t rfm69_get_status(rfm69_t *dev, solar_os_radio_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = rfm69_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t opmode = 0;
    uint8_t rssi = 0;
    ret = rfm69_read_reg_locked(dev, REG_OP_MODE, &opmode);
    if (ret == ESP_OK) {
        ret = rfm69_read_reg_locked(dev, REG_RSSI_VALUE, &rssi);
    }
    if (ret == ESP_OK) {
        memset(status, 0, sizeof(*status));
        status->state = rfm69_state_from_opmode(opmode);
        status->config = dev->config;
        status->has_rssi = true;
        status->rssi_dbm = -(int16_t)(rssi / 2U);
        dev->state = status->state;
    }

    rfm69_unlock(dev);
    return ret;
}

esp_err_t rfm69_send(rfm69_t *dev, const solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    if (packet == NULL || packet->len == 0 || packet->len > RFM69_MAX_PACKET_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = rfm69_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!dev->config.variable_length) {
        rfm69_unlock(dev);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t fifo[RFM69_MAX_PACKET_LEN + 2];
    size_t fifo_len = 0;
    const bool send_address = packet->has_destination;
    const size_t radio_len = packet->len + (send_address ? 1U : 0U);
    if (radio_len > RFM69_MAX_PACKET_LEN + 1U) {
        rfm69_unlock(dev);
        return ESP_ERR_INVALID_SIZE;
    }

    fifo[fifo_len++] = (uint8_t)radio_len;
    if (send_address) {
        fifo[fifo_len++] = (uint8_t)packet->destination;
    }
    memcpy(&fifo[fifo_len], packet->data, packet->len);
    fifo_len += packet->len;

    bool tx_started = false;
    ret = rfm69_set_state_locked(dev, SOLAR_OS_RADIO_STATE_STANDBY);
    if (ret == ESP_OK) {
        ret = rfm69_write_reg_locked(dev, REG_IRQ_FLAGS2, IRQ2_FIFO_OVERRUN);
    }
    if (ret == ESP_OK) {
        ret = rfm69_write_burst_locked(dev, REG_FIFO, fifo, fifo_len);
    }
    if (ret == ESP_OK) {
        ret = rfm69_set_state_locked(dev, SOLAR_OS_RADIO_STATE_TX);
        tx_started = ret == ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = rfm69_wait_flag_locked(dev, REG_IRQ_FLAGS2, IRQ2_PACKET_SENT, timeout_ms);
    }
    if (tx_started) {
        const esp_err_t standby_ret = rfm69_set_state_locked(dev, SOLAR_OS_RADIO_STATE_STANDBY);
        if (ret == ESP_OK) {
            ret = standby_ret;
        }
    }

    rfm69_unlock(dev);
    return ret;
}

esp_err_t rfm69_receive(rfm69_t *dev, solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = rfm69_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }
    if (dev->state != SOLAR_OS_RADIO_STATE_RX) {
        ret = rfm69_set_state_locked(dev, SOLAR_OS_RADIO_STATE_RX);
    }
    if (ret == ESP_OK) {
        ret = rfm69_wait_flag_locked(dev, REG_IRQ_FLAGS2, IRQ2_PAYLOAD_READY, timeout_ms);
    }
    if (ret != ESP_OK) {
        rfm69_unlock(dev);
        return ret;
    }

    uint8_t irq2 = 0;
    uint8_t rssi = 0;
    uint8_t radio_len = (uint8_t)(dev->config.payload_length +
                                  (dev->config.has_node_id ? 1U : 0U));
    ret = rfm69_read_reg_locked(dev, REG_IRQ_FLAGS2, &irq2);
    if (ret == ESP_OK) {
        ret = rfm69_read_reg_locked(dev, REG_RSSI_VALUE, &rssi);
    }
    if (ret == ESP_OK && dev->config.variable_length) {
        ret = rfm69_read_burst_locked(dev, REG_FIFO, &radio_len, 1);
    }
    if (ret != ESP_OK) {
        rfm69_unlock(dev);
        return ret;
    }

    const bool has_address = dev->config.has_node_id;
    if (radio_len == 0 || radio_len > RFM69_MAX_PACKET_LEN + (has_address ? 1U : 0U)) {
        (void)rfm69_write_reg_locked(dev, REG_IRQ_FLAGS2, IRQ2_FIFO_OVERRUN);
        rfm69_unlock(dev);
        return ESP_ERR_INVALID_SIZE;
    }

    memset(packet, 0, sizeof(*packet));
    size_t data_len = radio_len;
    if (has_address) {
        uint8_t address = 0;
        ret = rfm69_read_burst_locked(dev, REG_FIFO, &address, 1);
        if (ret != ESP_OK) {
            rfm69_unlock(dev);
            return ret;
        }
        packet->has_destination = true;
        packet->destination = address;
        data_len--;
    }
    if (data_len > RFM69_MAX_PACKET_LEN) {
        (void)rfm69_write_reg_locked(dev, REG_IRQ_FLAGS2, IRQ2_FIFO_OVERRUN);
        rfm69_unlock(dev);
        return ESP_ERR_INVALID_SIZE;
    }
    if (data_len > 0) {
        ret = rfm69_read_burst_locked(dev, REG_FIFO, packet->data, data_len);
    }
    if (ret == ESP_OK) {
        packet->len = data_len;
        packet->has_rssi = true;
        packet->rssi_dbm = -(int16_t)(rssi / 2U);
        packet->crc_ok = !dev->config.crc_enabled || ((irq2 & IRQ2_CRC_OK) != 0);
    }

    rfm69_unlock(dev);
    return ret;
}
