#include "rfm95.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_spi.h"

#define RFM95_FXOSC_HZ 32000000ULL
#define RFM95_FSTEP_DEN 524288ULL

#define REG_FIFO 0x00
#define REG_OP_MODE 0x01
#define REG_FSK_BITRATE_MSB 0x02
#define REG_FSK_BITRATE_LSB 0x03
#define REG_FSK_FDEV_MSB 0x04
#define REG_FSK_FDEV_LSB 0x05
#define REG_FRF_MSB 0x06
#define REG_FRF_MID 0x07
#define REG_FRF_LSB 0x08
#define REG_PA_CONFIG 0x09
#define REG_PA_RAMP 0x0A
#define REG_OCP 0x0B
#define REG_LNA 0x0C
#define REG_FSK_RX_CONFIG 0x0D
#define REG_FIFO_ADDR_PTR 0x0D
#define REG_FIFO_TX_BASE_ADDR 0x0E
#define REG_FIFO_RX_BASE_ADDR 0x0F
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_FSK_RSSI_VALUE 0x11
#define REG_FSK_RX_BW 0x12
#define REG_IRQ_FLAGS 0x12
#define REG_FSK_AFC_BW 0x13
#define REG_RX_NB_BYTES 0x13
#define REG_PKT_SNR_VALUE 0x19
#define REG_PKT_RSSI_VALUE 0x1A
#define REG_RSSI_VALUE 0x1B
#define REG_MODEM_CONFIG1 0x1D
#define REG_MODEM_CONFIG2 0x1E
#define REG_SYMB_TIMEOUT_LSB 0x1F
#define REG_PREAMBLE_MSB 0x20
#define REG_PREAMBLE_LSB 0x21
#define REG_PAYLOAD_LENGTH 0x22
#define REG_MAX_PAYLOAD_LENGTH 0x23
#define REG_MODEM_CONFIG3 0x26
#define REG_FSK_PREAMBLE_MSB 0x25
#define REG_FSK_PREAMBLE_LSB 0x26
#define REG_FSK_SYNC_CONFIG 0x27
#define REG_FSK_SYNC_VALUE1 0x28
#define REG_FSK_PACKET_CONFIG1 0x30
#define REG_FSK_PACKET_CONFIG2 0x31
#define REG_FSK_PAYLOAD_LENGTH 0x32
#define REG_FSK_NODE_ADDRESS 0x33
#define REG_FSK_BROADCAST_ADDRESS 0x34
#define REG_FSK_FIFO_THRESH 0x35
#define REG_DETECT_OPTIMIZE 0x31
#define REG_DETECTION_THRESHOLD 0x37
#define REG_SYNC_WORD 0x39
#define REG_FSK_IRQ_FLAGS1 0x3E
#define REG_FSK_IRQ_FLAGS2 0x3F
#define REG_DIO_MAPPING1 0x40
#define REG_DIO_MAPPING2 0x41
#define REG_VERSION 0x42
#define REG_PA_DAC 0x4D

#define OP_MODE_LONG_RANGE 0x80
#define OP_MODE_OOK 0x20
#define OP_MODE_LOW_FREQUENCY 0x08
#define OP_MODE_SLEEP 0x00
#define OP_MODE_STANDBY 0x01
#define OP_MODE_TX 0x03
#define OP_MODE_RX_CONTINUOUS 0x05

#define IRQ_RX_TIMEOUT 0x80
#define IRQ_RX_DONE 0x40
#define IRQ_PAYLOAD_CRC_ERROR 0x20
#define IRQ_TX_DONE 0x08

#define FSK_IRQ1_MODE_READY 0x80
#define FSK_IRQ2_FIFO_OVERRUN 0x10
#define FSK_IRQ2_FIFO_LEVEL 0x20
#define FSK_IRQ2_FIFO_NOT_EMPTY 0x40
#define FSK_IRQ2_PACKET_SENT 0x08
#define FSK_IRQ2_PAYLOAD_READY 0x04
#define FSK_IRQ2_CRC_OK 0x02

#define FSK_PACKET_VARIABLE 0x80
#define FSK_PACKET_CRC_ON 0x10
#define FSK_PACKET_ADDRESS_NODE 0x02
#define FSK_PACKET_MODE 0x40

#define SPI_WRITE_BIT 0x80
#define RFM95_MODE_SETTLE_MS 1
#define RFM95_FSK_MODE_WAIT_MS 100
#define RFM95_FSK_FIFO_CAPACITY 66
#define RFM95_FSK_FIFO_DRAIN_LEN 16

static esp_err_t rfm95_wait_reg_locked(rfm95_t *dev,
                                       uint8_t reg,
                                       uint8_t mask,
                                       bool any,
                                       uint32_t timeout_ms,
                                       uint8_t *value_out);

static esp_err_t rfm95_lock(rfm95_t *dev)
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

static void rfm95_unlock(rfm95_t *dev)
{
    if (dev != NULL && dev->mutex != NULL) {
        xSemaphoreGive(dev->mutex);
    }
}

static esp_err_t rfm95_transfer(rfm95_t *dev,
                                const uint8_t *tx,
                                uint8_t *rx,
                                size_t len)
{
    return solar_os_bus_spi_transfer(dev->spi_bus,
                                     dev->cs_pin,
                                     0,
                                     dev->speed_hz,
                                     tx,
                                     rx,
                                     len);
}

static esp_err_t rfm95_read_reg_locked(rfm95_t *dev, uint8_t reg, uint8_t *value)
{
    uint8_t tx[2] = {(uint8_t)(reg & 0x7F), 0};
    uint8_t rx[2] = {0};

    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ret = rfm95_transfer(dev, tx, rx, sizeof(tx));
    if (ret == ESP_OK) {
        *value = rx[1];
    }
    return ret;
}

static esp_err_t rfm95_write_reg_locked(rfm95_t *dev, uint8_t reg, uint8_t value)
{
    const uint8_t tx[2] = {(uint8_t)(reg | SPI_WRITE_BIT), value};
    return rfm95_transfer(dev, tx, NULL, sizeof(tx));
}

static esp_err_t rfm95_write_burst_locked(rfm95_t *dev,
                                          uint8_t reg,
                                          const uint8_t *data,
                                          size_t len)
{
    uint8_t tx[RFM95_MAX_PACKET_LEN + 1];

    if (data == NULL || len == 0 || len > RFM95_MAX_PACKET_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    tx[0] = (uint8_t)(reg | SPI_WRITE_BIT);
    memcpy(&tx[1], data, len);
    return rfm95_transfer(dev, tx, NULL, len + 1);
}

static esp_err_t rfm95_read_burst_locked(rfm95_t *dev,
                                         uint8_t reg,
                                         uint8_t *data,
                                         size_t len)
{
    uint8_t tx[RFM95_MAX_PACKET_LEN + 1] = {0};
    uint8_t rx[RFM95_MAX_PACKET_LEN + 1] = {0};

    if (data == NULL || len == 0 || len > RFM95_MAX_PACKET_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    tx[0] = (uint8_t)(reg & 0x7F);
    const esp_err_t ret = rfm95_transfer(dev, tx, rx, len + 1);
    if (ret == ESP_OK) {
        memcpy(data, &rx[1], len);
    }
    return ret;
}

static uint8_t rfm95_mode_bits(solar_os_radio_state_t state)
{
    switch (state) {
    case SOLAR_OS_RADIO_STATE_SLEEP:
        return OP_MODE_SLEEP;
    case SOLAR_OS_RADIO_STATE_STANDBY:
        return OP_MODE_STANDBY;
    case SOLAR_OS_RADIO_STATE_RX:
        return OP_MODE_RX_CONTINUOUS;
    case SOLAR_OS_RADIO_STATE_TX:
        return OP_MODE_TX;
    case SOLAR_OS_RADIO_STATE_UNKNOWN:
    default:
        return 0xFF;
    }
}

static bool rfm95_is_lora(const solar_os_radio_config_t *config)
{
    return config != NULL && config->modulation == SOLAR_OS_RADIO_MODULATION_LORA;
}

static uint8_t rfm95_op_mode_for_config(const solar_os_radio_config_t *config,
                                        solar_os_radio_state_t state)
{
    const bool low_frequency = config != NULL && config->frequency_hz < 525000000U;
    const bool lora = rfm95_is_lora(config);
    const bool ook = config != NULL && config->modulation == SOLAR_OS_RADIO_MODULATION_OOK;
    return (uint8_t)((lora ? OP_MODE_LONG_RANGE : 0) |
                     (ook ? OP_MODE_OOK : 0) |
                     (low_frequency ? OP_MODE_LOW_FREQUENCY : 0) |
                     rfm95_mode_bits(state));
}

static esp_err_t rfm95_set_state_for_config_locked(rfm95_t *dev,
                                                   const solar_os_radio_config_t *config,
                                                   solar_os_radio_state_t state)
{
    if (rfm95_mode_bits(state) == 0xFF) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret =
        rfm95_write_reg_locked(dev, REG_OP_MODE, rfm95_op_mode_for_config(config, state));
    if (ret == ESP_OK && state != SOLAR_OS_RADIO_STATE_SLEEP) {
        if (rfm95_is_lora(config)) {
            vTaskDelay(pdMS_TO_TICKS(RFM95_MODE_SETTLE_MS));
        } else {
            ret = rfm95_wait_reg_locked(dev,
                                        REG_FSK_IRQ_FLAGS1,
                                        FSK_IRQ1_MODE_READY,
                                        false,
                                        RFM95_FSK_MODE_WAIT_MS,
                                        NULL);
        }
    }
    if (ret == ESP_OK) {
        dev->state = state;
    }
    return ret;
}

static esp_err_t rfm95_set_state_locked(rfm95_t *dev, solar_os_radio_state_t state)
{
    return rfm95_set_state_for_config_locked(dev, &dev->config, state);
}

static solar_os_radio_state_t rfm95_state_from_op_mode(uint8_t op_mode)
{
    switch (op_mode & 0x07) {
    case OP_MODE_SLEEP:
        return SOLAR_OS_RADIO_STATE_SLEEP;
    case OP_MODE_STANDBY:
        return SOLAR_OS_RADIO_STATE_STANDBY;
    case OP_MODE_TX:
        return SOLAR_OS_RADIO_STATE_TX;
    case OP_MODE_RX_CONTINUOUS:
        return SOLAR_OS_RADIO_STATE_RX;
    default:
        return SOLAR_OS_RADIO_STATE_UNKNOWN;
    }
}

static bool rfm95_lora_bandwidth_bits(uint32_t bandwidth_hz, uint8_t *bits)
{
    if (bits == NULL) {
        return false;
    }
    switch (bandwidth_hz) {
    case 62500:
        *bits = 6;
        return true;
    case 125000:
        *bits = 7;
        return true;
    case 250000:
        *bits = 8;
        return true;
    case 500000:
        *bits = 9;
        return true;
    default:
        return false;
    }
}

static bool rfm95_fsk_bandwidth_value(uint32_t bandwidth_hz, uint8_t *value)
{
    if (bandwidth_hz == 0 || value == NULL) {
        return false;
    }

    static const uint8_t mantissas[] = {16, 20, 24};
    uint32_t best_hz = UINT32_MAX;
    uint8_t best_value = 0;

    for (uint8_t mantissa_code = 0;
         mantissa_code < sizeof(mantissas) / sizeof(mantissas[0]);
         mantissa_code++) {
        for (uint8_t exponent = 1; exponent <= 7; exponent++) {
            const uint32_t actual_hz =
                (uint32_t)(RFM95_FXOSC_HZ /
                           ((uint64_t)mantissas[mantissa_code] << (exponent + 2U)));
            if (actual_hz >= bandwidth_hz && actual_hz < best_hz) {
                best_hz = actual_hz;
                best_value = (uint8_t)((mantissa_code << 3) | exponent);
            }
        }
    }

    if (best_hz == UINT32_MAX) {
        return false;
    }
    *value = best_value;
    return true;
}

static bool rfm95_fsk_modulation(solar_os_radio_modulation_t modulation)
{
    switch (modulation) {
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

static bool rfm95_lora_config_valid(const solar_os_radio_config_t *config)
{
    uint8_t bandwidth = 0;

    if (!rfm95_is_lora(config) ||
        !rfm95_lora_bandwidth_bits(config->rx_bandwidth_hz, &bandwidth) ||
        config->spreading_factor < 6 ||
        config->spreading_factor > 12 ||
        config->coding_rate_denominator < 5 ||
        config->coding_rate_denominator > 8 ||
        config->preamble_len < 6 ||
        config->sync_word_len != 1 ||
        config->tx_power_dbm < 2 ||
        config->tx_power_dbm > 20 ||
        config->payload_length == 0 ||
        config->payload_length > RFM95_MAX_PACKET_LEN ||
        config->has_node_id ||
        config->has_network_id) {
        return false;
    }
    if (config->spreading_factor == 6 && config->variable_length) {
        return false;
    }
    return true;
}

static bool rfm95_fsk_config_valid(const solar_os_radio_config_t *config)
{
    uint8_t bandwidth = 0;

    if (config == NULL ||
        !rfm95_fsk_modulation(config->modulation) ||
        config->bitrate_bps < 1200U ||
        config->bitrate_bps > (config->modulation == SOLAR_OS_RADIO_MODULATION_OOK
                                   ? 32768U
                                   : 300000U) ||
        !rfm95_fsk_bandwidth_value(config->rx_bandwidth_hz, &bandwidth) ||
        config->bitrate_bps > config->rx_bandwidth_hz * 2U ||
        config->sync_word_len > SOLAR_OS_RADIO_SYNC_WORD_MAX ||
        config->tx_power_dbm < 2 ||
        config->tx_power_dbm > 20 ||
        config->payload_length > RFM95_FSK_MAX_PACKET_LEN ||
        config->has_network_id) {
        return false;
    }
    if (config->modulation != SOLAR_OS_RADIO_MODULATION_OOK &&
        (config->deviation_hz < 600U ||
         config->deviation_hz > 200000U ||
         config->deviation_hz + config->bitrate_bps / 2U > 250000U)) {
        return false;
    }
    if (config->payload_length == 0 &&
        (config->variable_length || config->crc_enabled || config->has_node_id)) {
        return false;
    }
    return true;
}

static bool rfm95_config_valid(const solar_os_radio_config_t *config)
{
    if (config == NULL ||
        config->frequency_hz < 862000000U ||
        config->frequency_hz > 1020000000U) {
        return false;
    }
    return rfm95_is_lora(config) ? rfm95_lora_config_valid(config)
                                 : rfm95_fsk_config_valid(config);
}

static void rfm95_resolve_config(const rfm95_t *dev,
                                 const solar_os_radio_config_t *requested,
                                 solar_os_radio_config_t *resolved)
{
    *resolved = *requested;
    if (resolved->rx_gain == SOLAR_OS_RADIO_RX_GAIN_DEFAULT) {
        resolved->rx_gain = SOLAR_OS_RADIO_RX_GAIN_BOOSTED;
    }

    if (rfm95_is_lora(resolved)) {
        uint8_t bandwidth = 0;
        if (!rfm95_lora_bandwidth_bits(resolved->rx_bandwidth_hz, &bandwidth)) {
            resolved->rx_bandwidth_hz = 125000U;
        }
        if (resolved->spreading_factor < 6 || resolved->spreading_factor > 12) {
            resolved->spreading_factor = 7;
        }
        if (resolved->coding_rate_denominator < 5 ||
            resolved->coding_rate_denominator > 8) {
            resolved->coding_rate_denominator = 5;
        }
        if (resolved->preamble_len < 6) {
            resolved->preamble_len = 8;
        }
        if (resolved->sync_word_len != 1) {
            resolved->sync_word_len = 1;
            resolved->sync_word[0] = 0x12;
        }
        if (resolved->payload_length == 0 ||
            resolved->payload_length > RFM95_MAX_PACKET_LEN) {
            resolved->payload_length = RFM95_MAX_PACKET_LEN;
        }
        resolved->has_node_id = false;
        resolved->has_network_id = false;
        return;
    }

    if (!rfm95_fsk_modulation(resolved->modulation)) {
        return;
    }
    if (resolved->bitrate_bps == 0) {
        resolved->bitrate_bps = 4800U;
    }
    if (resolved->modulation == SOLAR_OS_RADIO_MODULATION_MSK ||
        resolved->modulation == SOLAR_OS_RADIO_MODULATION_GMSK) {
        resolved->deviation_hz = resolved->bitrate_bps / 4U;
    } else if (resolved->modulation == SOLAR_OS_RADIO_MODULATION_OOK) {
        resolved->deviation_hz = 0;
    } else if (resolved->deviation_hz == 0) {
        resolved->deviation_hz = 5000U;
    }
    if (resolved->rx_bandwidth_hz == 0) {
        resolved->rx_bandwidth_hz = 12500U;
    }
    if (resolved->payload_length > RFM95_FSK_MAX_PACKET_LEN &&
        dev != NULL && rfm95_is_lora(&dev->config)) {
        resolved->payload_length = RFM95_FSK_MAX_PACKET_LEN;
    }
}

static uint32_t rfm95_frequency_reg(uint32_t frequency_hz)
{
    return (uint32_t)(((uint64_t)frequency_hz * RFM95_FSTEP_DEN +
                       RFM95_FXOSC_HZ / 2ULL) /
                      RFM95_FXOSC_HZ);
}

static uint16_t rfm95_bitrate_reg(uint32_t bitrate_bps)
{
    uint64_t value = (RFM95_FXOSC_HZ + bitrate_bps / 2U) / bitrate_bps;
    if (value == 0) {
        value = 1;
    }
    if (value > UINT16_MAX) {
        value = UINT16_MAX;
    }
    return (uint16_t)value;
}

static uint16_t rfm95_deviation_reg(uint32_t deviation_hz)
{
    uint64_t value = ((uint64_t)deviation_hz * RFM95_FSTEP_DEN +
                      RFM95_FXOSC_HZ / 2U) /
                     RFM95_FXOSC_HZ;
    if (value > 0x3FFFU) {
        value = 0x3FFFU;
    }
    return (uint16_t)value;
}

static uint8_t rfm95_pa_ramp_value(solar_os_radio_modulation_t modulation)
{
    switch (modulation) {
    case SOLAR_OS_RADIO_MODULATION_GFSK:
    case SOLAR_OS_RADIO_MODULATION_GMSK:
        return 0x49; /* Gaussian shaping, BT=1.0; 40 us PA ramp. */
    case SOLAR_OS_RADIO_MODULATION_FSK:
    case SOLAR_OS_RADIO_MODULATION_MSK:
    case SOLAR_OS_RADIO_MODULATION_OOK:
    case SOLAR_OS_RADIO_MODULATION_NONE:
    case SOLAR_OS_RADIO_MODULATION_LORA:
    default:
        return 0x09;
    }
}

static bool rfm95_low_data_rate_optimize(const solar_os_radio_config_t *config)
{
    const uint64_t symbol_us =
        ((uint64_t)1U << config->spreading_factor) * 1000000ULL /
        config->rx_bandwidth_hz;
    return solar_os_radio_low_data_rate(config, symbol_us > 16000ULL);
}

static esp_err_t rfm95_configure_power_locked(rfm95_t *dev, int8_t power_dbm)
{
    uint8_t output_power = 0;
    uint8_t pa_dac = 0x84;
    uint8_t ocp = 0x2B;

    if (power_dbm > 17) {
        output_power = (uint8_t)(power_dbm - 5);
        pa_dac = 0x87;
        ocp = 0x31;
    } else {
        output_power = (uint8_t)(power_dbm - 2);
    }

    esp_err_t ret = rfm95_write_reg_locked(dev,
                                           REG_PA_CONFIG,
                                           (uint8_t)(0x80 | (output_power & 0x0F)));
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_PA_DAC, pa_dac);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_OCP, ocp);
    }
    return ret;
}

static esp_err_t rfm95_wait_reg_locked(rfm95_t *dev,
                                       uint8_t reg,
                                       uint8_t mask,
                                       bool any,
                                       uint32_t timeout_ms,
                                       uint8_t *value_out)
{
    const int64_t start_us = esp_timer_get_time();
    const int64_t timeout_us = (int64_t)timeout_ms * 1000;

    while (true) {
        uint8_t value = 0;
        const esp_err_t ret = rfm95_read_reg_locked(dev, reg, &value);
        if (ret != ESP_OK) {
            return ret;
        }
        if ((any && (value & mask) != 0) || (!any && (value & mask) == mask)) {
            if (value_out != NULL) {
                *value_out = value;
            }
            return ESP_OK;
        }
        if (timeout_ms == 0 || esp_timer_get_time() - start_us >= timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static esp_err_t rfm95_wait_lora_irq_locked(rfm95_t *dev,
                                            uint8_t mask,
                                            uint32_t timeout_ms,
                                            uint8_t *flags)
{
    return rfm95_wait_reg_locked(dev,
                                 REG_IRQ_FLAGS,
                                 mask,
                                 true,
                                 timeout_ms,
                                 flags);
}

static esp_err_t rfm95_enter_modem_sleep_locked(rfm95_t *dev,
                                                const solar_os_radio_config_t *config)
{
    uint8_t current = 0;
    esp_err_t ret = rfm95_read_reg_locked(dev, REG_OP_MODE, &current);
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_OP_MODE,
                                     (uint8_t)(current & (OP_MODE_LONG_RANGE |
                                                         OP_MODE_OOK |
                                                         OP_MODE_LOW_FREQUENCY)));
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_OP_MODE,
                                     rfm95_op_mode_for_config(config,
                                                              SOLAR_OS_RADIO_STATE_SLEEP));
    }
    if (ret == ESP_OK) {
        dev->state = SOLAR_OS_RADIO_STATE_SLEEP;
    }
    return ret;
}

static esp_err_t rfm95_configure_common_locked(rfm95_t *dev,
                                               const solar_os_radio_config_t *config)
{
    const uint32_t frequency = rfm95_frequency_reg(config->frequency_hz);
    esp_err_t ret =
        rfm95_write_reg_locked(dev, REG_FRF_MSB, (uint8_t)(frequency >> 16));
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FRF_MID, (uint8_t)(frequency >> 8));
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FRF_LSB, (uint8_t)frequency);
    }
    if (ret == ESP_OK) {
        ret = rfm95_configure_power_locked(dev, config->tx_power_dbm);
    }
    if (ret == ESP_OK) {
        /* Automatic gain remains enabled; control HF LNA boost independently. */
        ret = rfm95_write_reg_locked(dev, REG_LNA,
            config->rx_gain == SOLAR_OS_RADIO_RX_GAIN_BOOSTED ? 0x23 : 0x20);
    }
    return ret;
}

static esp_err_t rfm95_configure_lora_locked(rfm95_t *dev,
                                             const solar_os_radio_config_t *config)
{
    uint8_t bandwidth = 0;
    if (!rfm95_lora_bandwidth_bits(config->rx_bandwidth_hz, &bandwidth)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = rfm95_enter_modem_sleep_locked(dev, config);
    if (ret == ESP_OK) {
        ret = rfm95_configure_common_locked(dev, config);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FIFO_TX_BASE_ADDR, 0x00);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FIFO_RX_BASE_ADDR, 0x00);
    }

    const uint8_t modem_config1 =
        (uint8_t)((bandwidth << 4) |
                  ((config->coding_rate_denominator - 4U) << 1) |
                  (config->variable_length ? 0 : 1));
    const uint8_t modem_config2 =
        (uint8_t)((config->spreading_factor << 4) |
                  (config->crc_enabled ? 0x04 : 0) |
                  0x03);
    const uint8_t modem_config3 =
        (uint8_t)(0x04 | (rfm95_low_data_rate_optimize(config) ? 0x08 : 0));

    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_MODEM_CONFIG1, modem_config1);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_MODEM_CONFIG2, modem_config2);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_MODEM_CONFIG3, modem_config3);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_SYMB_TIMEOUT_LSB, 0xFF);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_PREAMBLE_MSB,
                                     (uint8_t)(config->preamble_len >> 8));
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_PREAMBLE_LSB,
                                     (uint8_t)config->preamble_len);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_PAYLOAD_LENGTH,
                                     (uint8_t)config->payload_length);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_MAX_PAYLOAD_LENGTH, RFM95_MAX_PACKET_LEN);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_SYNC_WORD, config->sync_word[0]);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_DIO_MAPPING1, 0x00);
    }
    if (ret == ESP_OK) {
        const bool sf6 = config->spreading_factor == 6;
        ret = rfm95_write_reg_locked(dev, REG_DETECT_OPTIMIZE, sf6 ? 0x05 : 0x03);
        if (ret == ESP_OK) {
            ret = rfm95_write_reg_locked(dev, REG_DETECTION_THRESHOLD, sf6 ? 0x0C : 0x0A);
        }
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_IRQ_FLAGS, 0xFF);
    }
    if (ret == ESP_OK) {
        ret = rfm95_set_state_for_config_locked(dev,
                                                config,
                                                SOLAR_OS_RADIO_STATE_STANDBY);
    }
    return ret;
}

static esp_err_t rfm95_configure_fsk_locked(rfm95_t *dev,
                                            const solar_os_radio_config_t *config)
{
    uint8_t bandwidth = 0;
    if (!rfm95_fsk_bandwidth_value(config->rx_bandwidth_hz, &bandwidth)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t bitrate = rfm95_bitrate_reg(config->bitrate_bps);
    const uint16_t deviation = rfm95_deviation_reg(config->deviation_hz);
    const uint8_t radio_payload_length =
        (uint8_t)(config->payload_length + (config->has_node_id ? 1U : 0U));
    uint8_t packet_config1 = config->variable_length ? FSK_PACKET_VARIABLE : 0;
    if (config->crc_enabled) {
        packet_config1 |= FSK_PACKET_CRC_ON;
    }
    if (config->has_node_id) {
        packet_config1 |= FSK_PACKET_ADDRESS_NODE;
    }

    esp_err_t ret = rfm95_enter_modem_sleep_locked(dev, config);
    if (ret == ESP_OK) {
        ret = rfm95_configure_common_locked(dev, config);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_PA_RAMP,
                                     rfm95_pa_ramp_value(config->modulation));
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FSK_BITRATE_MSB,
                                     (uint8_t)(bitrate >> 8));
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FSK_BITRATE_LSB, (uint8_t)bitrate);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FSK_FDEV_MSB,
                                     (uint8_t)(deviation >> 8));
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FSK_FDEV_LSB, (uint8_t)deviation);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FSK_RX_CONFIG, 0x1E);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FSK_RX_BW, bandwidth);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FSK_AFC_BW, bandwidth);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_FSK_PREAMBLE_MSB,
                                     (uint8_t)(config->preamble_len >> 8));
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_FSK_PREAMBLE_LSB,
                                     (uint8_t)config->preamble_len);
    }
    if (ret == ESP_OK) {
        const uint8_t sync_config =
            config->sync_word_len == 0
                ? 0x80
                : (uint8_t)(0x90 | (config->sync_word_len - 1U));
        ret = rfm95_write_reg_locked(dev, REG_FSK_SYNC_CONFIG, sync_config);
    }
    for (uint8_t i = 0; ret == ESP_OK && i < config->sync_word_len; i++) {
        ret = rfm95_write_reg_locked(dev,
                                     (uint8_t)(REG_FSK_SYNC_VALUE1 + i),
                                     config->sync_word[i]);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FSK_PACKET_CONFIG1, packet_config1);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FSK_PACKET_CONFIG2, FSK_PACKET_MODE);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_FSK_PAYLOAD_LENGTH,
                                     radio_payload_length);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_FSK_NODE_ADDRESS,
                                     config->has_node_id ? config->node_id : 0);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FSK_BROADCAST_ADDRESS, 0xFF);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FSK_FIFO_THRESH, 0x8F);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_DIO_MAPPING1, 0x00);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_DIO_MAPPING2, 0x00);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_FSK_IRQ_FLAGS2,
                                     FSK_IRQ2_FIFO_OVERRUN);
    }
    if (ret == ESP_OK) {
        ret = rfm95_set_state_for_config_locked(dev,
                                                config,
                                                SOLAR_OS_RADIO_STATE_STANDBY);
    }
    return ret;
}

esp_err_t rfm95_init(rfm95_t *dev,
                     const char *spi_bus,
                     int cs_pin,
                     uint32_t speed_hz)
{
    if (dev == NULL || spi_bus == NULL || spi_bus[0] == '\0' ||
        strnlen(spi_bus, sizeof(dev->spi_bus)) >= sizeof(dev->spi_bus) ||
        cs_pin < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (dev->mutex == NULL) {
        dev->mutex = xSemaphoreCreateMutex();
        if (dev->mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    strlcpy(dev->spi_bus, spi_bus, sizeof(dev->spi_bus));
    dev->cs_pin = cs_pin;
    dev->speed_hz = speed_hz != 0 ? speed_hz : SOLAR_OS_SPI_DEFAULT_SPEED_HZ;
    dev->state = SOLAR_OS_RADIO_STATE_UNKNOWN;
    return ESP_OK;
}

esp_err_t rfm95_probe(rfm95_t *dev, uint8_t *version)
{
    esp_err_t ret = rfm95_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t value = 0;
    ret = rfm95_read_reg_locked(dev, REG_VERSION, &value);
    rfm95_unlock(dev);
    if (ret != ESP_OK) {
        return ret;
    }
    if (version != NULL) {
        *version = value;
    }
    return value == RFM95_VERSION ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t rfm95_configure(rfm95_t *dev, const solar_os_radio_config_t *config)
{
    if (dev == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->rx_gain != SOLAR_OS_RADIO_RX_GAIN_DEFAULT &&
        config->rx_gain != SOLAR_OS_RADIO_RX_GAIN_POWER_SAVING &&
        config->rx_gain != SOLAR_OS_RADIO_RX_GAIN_BOOSTED) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    solar_os_radio_config_t resolved;
    rfm95_resolve_config(dev, config, &resolved);
    if (!rfm95_config_valid(&resolved)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = rfm95_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = rfm95_is_lora(&resolved)
              ? rfm95_configure_lora_locked(dev, &resolved)
              : rfm95_configure_fsk_locked(dev, &resolved);
    if (ret == ESP_OK) {
        dev->config = resolved;
        dev->has_last_packet = false;
    }

    rfm95_unlock(dev);
    return ret;
}

esp_err_t rfm95_set_state(rfm95_t *dev, solar_os_radio_state_t state)
{
    esp_err_t ret = rfm95_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = rfm95_set_state_locked(dev, state);
    rfm95_unlock(dev);
    return ret;
}

esp_err_t rfm95_tune_rx(rfm95_t *dev, uint32_t frequency_hz, uint32_t *settle_ms)
{
    if (dev == NULL || settle_ms == NULL || frequency_hz < 862000000U ||
        frequency_hz > 1020000000U) return ESP_ERR_INVALID_ARG;
    esp_err_t err = rfm95_lock(dev);
    if (err != ESP_OK) return err;
    if (dev->state != SOLAR_OS_RADIO_STATE_RX && dev->state != SOLAR_OS_RADIO_STATE_STANDBY) {
        rfm95_unlock(dev);
        return ESP_ERR_INVALID_STATE;
    }
    err = rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_STANDBY);
    if (err == ESP_OK) {
        uint32_t reg = rfm95_frequency_reg(frequency_hz);
        uint8_t data[3] = {reg >> 16, reg >> 8, reg};
        err = rfm95_write_burst_locked(dev, REG_FRF_MSB, data, sizeof(data));
    }
    if (err == ESP_OK) err = rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_RX);
    if (err == ESP_OK) {
        dev->config.frequency_hz = frequency_hz;
        uint32_t bw = dev->config.rx_bandwidth_hz ? dev->config.rx_bandwidth_hz : 125000U;
        uint32_t filter_ms = (32000U + bw - 1U) / bw;
        *settle_ms = filter_ms < 2U ? 2U : filter_ms;
    }
    rfm95_unlock(dev);
    return err;
}

esp_err_t rfm95_get_status(rfm95_t *dev, solar_os_radio_status_t *status)
{
    if (dev == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = rfm95_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t op_mode = 0;
    uint8_t rssi = 0;
    ret = rfm95_read_reg_locked(dev, REG_OP_MODE, &op_mode);
    if (ret == ESP_OK) {
        ret = rfm95_read_reg_locked(dev,
                                    rfm95_is_lora(&dev->config)
                                        ? REG_RSSI_VALUE
                                        : REG_FSK_RSSI_VALUE,
                                    &rssi);
    }
    if (ret == ESP_OK) {
        memset(status, 0, sizeof(*status));
        status->state = rfm95_state_from_op_mode(op_mode);
        status->config = dev->config;
        status->has_rssi = true;
        if (rfm95_is_lora(&dev->config)) {
            status->rssi_dbm = (int16_t)(-157 + rssi);
            status->has_snr = dev->has_last_packet;
            status->snr_db = dev->last_snr_db;
        } else {
            status->rssi_dbm = -(int16_t)(rssi / 2U);
        }
        dev->state = status->state;
    }

    rfm95_unlock(dev);
    return ret;
}

static esp_err_t rfm95_send_lora_locked(rfm95_t *dev,
                                        const solar_os_radio_packet_t *packet,
                                        uint32_t timeout_ms)
{
    if (packet->len == 0 || packet->len > RFM95_MAX_PACKET_LEN ||
        packet->has_source || packet->has_destination) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!dev->config.variable_length && packet->len != dev->config.payload_length) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_STANDBY);
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_IRQ_FLAGS, 0xFF);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FIFO_ADDR_PTR, 0x00);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_PAYLOAD_LENGTH, (uint8_t)packet->len);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_burst_locked(dev, REG_FIFO, packet->data, packet->len);
    }
    if (ret == ESP_OK) {
        ret = rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_TX);
    }
    if (ret == ESP_OK) {
        ret = rfm95_wait_lora_irq_locked(dev, IRQ_TX_DONE, timeout_ms, NULL);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_IRQ_FLAGS, IRQ_TX_DONE);
    }

    const esp_err_t standby_ret =
        rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_STANDBY);
    if (ret == ESP_OK) {
        ret = standby_ret;
    }
    return ret;
}

static esp_err_t rfm95_send_fsk_locked(rfm95_t *dev,
                                       const solar_os_radio_packet_t *packet,
                                       uint32_t timeout_ms)
{
    if (packet->len == 0 || packet->len > RFM95_FSK_MAX_PACKET_LEN ||
        packet->has_source ||
        (packet->has_destination && packet->destination > UINT8_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!dev->config.variable_length &&
        (packet->len != dev->config.payload_length ||
         packet->has_destination != dev->config.has_node_id)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t fifo[RFM95_FSK_FIFO_CAPACITY];
    size_t fifo_len = 0;
    const size_t radio_len = packet->len + (packet->has_destination ? 1U : 0U);
    if (dev->config.variable_length) {
        fifo[fifo_len++] = (uint8_t)radio_len;
    }
    if (packet->has_destination) {
        fifo[fifo_len++] = (uint8_t)packet->destination;
    }
    memcpy(&fifo[fifo_len], packet->data, packet->len);
    fifo_len += packet->len;
    if (fifo_len > sizeof(fifo)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_STANDBY);
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_FSK_IRQ_FLAGS2,
                                     FSK_IRQ2_FIFO_OVERRUN);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_burst_locked(dev, REG_FIFO, fifo, fifo_len);
    }
    if (ret == ESP_OK) {
        ret = rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_TX);
    }
    if (ret == ESP_OK) {
        ret = rfm95_wait_reg_locked(dev,
                                    REG_FSK_IRQ_FLAGS2,
                                    FSK_IRQ2_PACKET_SENT,
                                    false,
                                    timeout_ms,
                                    NULL);
    }

    const esp_err_t standby_ret =
        rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_STANDBY);
    if (ret == ESP_OK) {
        ret = standby_ret;
    }
    return ret;
}

esp_err_t rfm95_send(rfm95_t *dev,
                     const solar_os_radio_packet_t *packet,
                     uint32_t timeout_ms)
{
    if (dev == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = rfm95_lock(dev);
    if (ret == ESP_OK) {
        ret = rfm95_is_lora(&dev->config)
                  ? rfm95_send_lora_locked(dev, packet, timeout_ms)
                  : rfm95_send_fsk_locked(dev, packet, timeout_ms);
        rfm95_unlock(dev);
    }
    return ret;
}

esp_err_t rfm95_send_stream(rfm95_t *dev,
                            const uint8_t *data,
                            size_t len,
                            uint32_t timeout_ms)
{
    if (dev == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = rfm95_lock(dev);
    if (ret != ESP_OK) {
        return ret;
    }
    if (rfm95_is_lora(&dev->config) ||
        dev->config.variable_length ||
        dev->config.payload_length != 0 ||
        dev->config.crc_enabled ||
        dev->config.has_node_id) {
        rfm95_unlock(dev);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const int64_t start_us = esp_timer_get_time();
    const int64_t timeout_us = (int64_t)timeout_ms * 1000;
    size_t offset = 0;
    bool tx_started = false;

    ret = rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_STANDBY);
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_FSK_IRQ_FLAGS2,
                                     FSK_IRQ2_FIFO_OVERRUN);
    }
    if (ret == ESP_OK) {
        const size_t initial =
            len < RFM95_FSK_FIFO_CAPACITY ? len : RFM95_FSK_FIFO_CAPACITY;
        ret = rfm95_write_burst_locked(dev, REG_FIFO, data, initial);
        offset = ret == ESP_OK ? initial : 0;
    }
    if (ret == ESP_OK) {
        ret = rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_TX);
        tx_started = ret == ESP_OK;
    }

    while (ret == ESP_OK && offset < len) {
        uint8_t flags = 0;
        ret = rfm95_read_reg_locked(dev, REG_FSK_IRQ_FLAGS2, &flags);
        if (ret != ESP_OK) {
            break;
        }
        if ((flags & FSK_IRQ2_FIFO_LEVEL) == 0) {
            size_t chunk = len - offset;
            if (chunk > RFM95_FSK_FIFO_DRAIN_LEN) {
                chunk = RFM95_FSK_FIFO_DRAIN_LEN;
            }
            ret = rfm95_write_burst_locked(dev, REG_FIFO, &data[offset], chunk);
            if (ret == ESP_OK) {
                offset += chunk;
            }
            continue;
        }
        if (timeout_ms == 0 || esp_timer_get_time() - start_us >= timeout_us) {
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    while (ret == ESP_OK) {
        uint8_t flags = 0;
        ret = rfm95_read_reg_locked(dev, REG_FSK_IRQ_FLAGS2, &flags);
        if (ret != ESP_OK || (flags & FSK_IRQ2_FIFO_NOT_EMPTY) == 0) {
            break;
        }
        if (timeout_ms == 0 || esp_timer_get_time() - start_us >= timeout_us) {
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (ret == ESP_OK) {
        const uint32_t tail_ms =
            (uint32_t)((16000ULL + dev->config.bitrate_bps - 1U) /
                       dev->config.bitrate_bps) +
            1U;
        vTaskDelay(pdMS_TO_TICKS(tail_ms));
    }
    if (tx_started) {
        const esp_err_t standby_ret =
            rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_STANDBY);
        if (ret == ESP_OK) {
            ret = standby_ret;
        }
    }
    rfm95_unlock(dev);
    return ret;
}

static esp_err_t rfm95_receive_lora_locked(rfm95_t *dev,
                                           solar_os_radio_packet_t *packet,
                                           uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    if (dev->state != SOLAR_OS_RADIO_STATE_RX) {
        ret = rfm95_write_reg_locked(dev, REG_IRQ_FLAGS, 0xFF);
        if (ret == ESP_OK) {
            ret = rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_RX);
        }
    }

    uint8_t flags = 0;
    if (ret == ESP_OK) {
        ret = rfm95_wait_lora_irq_locked(dev,
                                         IRQ_RX_DONE | IRQ_RX_TIMEOUT,
                                         timeout_ms,
                                         &flags);
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if ((flags & IRQ_RX_TIMEOUT) != 0) {
        (void)rfm95_write_reg_locked(dev, REG_IRQ_FLAGS, IRQ_RX_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t len = 0;
    uint8_t fifo_addr = 0;
    uint8_t raw_snr = 0;
    uint8_t raw_rssi = 0;
    ret = rfm95_read_reg_locked(dev, REG_RX_NB_BYTES, &len);
    if (ret == ESP_OK) {
        ret = rfm95_read_reg_locked(dev, REG_FIFO_RX_CURRENT_ADDR, &fifo_addr);
    }
    if (ret == ESP_OK) {
        ret = rfm95_write_reg_locked(dev, REG_FIFO_ADDR_PTR, fifo_addr);
    }
    if (ret == ESP_OK) {
        ret = rfm95_read_reg_locked(dev, REG_PKT_SNR_VALUE, &raw_snr);
    }
    if (ret == ESP_OK) {
        ret = rfm95_read_reg_locked(dev, REG_PKT_RSSI_VALUE, &raw_rssi);
    }
    if (ret == ESP_OK && len == 0) {
        ret = ESP_ERR_INVALID_SIZE;
    }

    memset(packet, 0, sizeof(*packet));
    if (ret == ESP_OK) {
        ret = rfm95_read_burst_locked(dev, REG_FIFO, packet->data, len);
    }
    if (ret == ESP_OK) {
        const int16_t snr_quarters = (int8_t)raw_snr;
        const int16_t snr_db = snr_quarters / 4;
        int16_t rssi_dbm = (int16_t)(-157 + raw_rssi);
        if (snr_quarters < 0) {
            rssi_dbm += snr_db;
        }

        packet->len = len;
        packet->has_rssi = true;
        packet->rssi_dbm = rssi_dbm;
        packet->has_snr = true;
        packet->snr_db = snr_db;
        packet->crc_ok = (flags & IRQ_PAYLOAD_CRC_ERROR) == 0;
        dev->last_rssi_dbm = rssi_dbm;
        dev->last_snr_db = snr_db;
        dev->has_last_packet = true;
    }

    const esp_err_t clear_ret =
        rfm95_write_reg_locked(dev,
                               REG_IRQ_FLAGS,
                               IRQ_RX_DONE | IRQ_RX_TIMEOUT | IRQ_PAYLOAD_CRC_ERROR);
    if (ret == ESP_OK) {
        ret = clear_ret;
    }
    return ret;
}

static esp_err_t rfm95_receive_fsk_stream_locked(rfm95_t *dev,
                                                 solar_os_radio_packet_t *packet,
                                                 uint32_t timeout_ms)
{
    const int64_t start_us = esp_timer_get_time();
    const int64_t timeout_us = (int64_t)timeout_ms * 1000;

    while (true) {
        uint8_t flags = 0;
        esp_err_t ret = rfm95_read_reg_locked(dev, REG_FSK_IRQ_FLAGS2, &flags);
        if (ret != ESP_OK) {
            return ret;
        }
        if ((flags & FSK_IRQ2_FIFO_OVERRUN) != 0) {
            (void)rfm95_write_reg_locked(dev,
                                         REG_FSK_IRQ_FLAGS2,
                                         FSK_IRQ2_FIFO_OVERRUN);
            return ESP_ERR_INVALID_SIZE;
        }

        size_t chunk_len = 0;
        if ((flags & FSK_IRQ2_FIFO_LEVEL) != 0) {
            chunk_len = RFM95_FSK_FIFO_DRAIN_LEN;
        } else if ((flags & FSK_IRQ2_FIFO_NOT_EMPTY) != 0) {
            chunk_len = 1;
        }
        if (chunk_len > 0) {
            uint8_t rssi = 0;
            memset(packet, 0, sizeof(*packet));
            ret = rfm95_read_reg_locked(dev, REG_FSK_RSSI_VALUE, &rssi);
            if (ret == ESP_OK) {
                ret = rfm95_read_burst_locked(dev,
                                              REG_FIFO,
                                              packet->data,
                                              chunk_len);
            }
            if (ret == ESP_OK) {
                packet->len = chunk_len;
                packet->has_rssi = true;
                packet->rssi_dbm = -(int16_t)(rssi / 2U);
                packet->crc_ok = true;
            }
            return ret;
        }

        if (timeout_ms == 0 || esp_timer_get_time() - start_us >= timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static esp_err_t rfm95_receive_fsk_locked(rfm95_t *dev,
                                          solar_os_radio_packet_t *packet,
                                          uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    if (dev->state != SOLAR_OS_RADIO_STATE_RX) {
        ret = rfm95_write_reg_locked(dev,
                                     REG_FSK_IRQ_FLAGS2,
                                     FSK_IRQ2_FIFO_OVERRUN);
        if (ret == ESP_OK) {
            ret = rfm95_set_state_locked(dev, SOLAR_OS_RADIO_STATE_RX);
        }
    }
    if (ret != ESP_OK) {
        return ret;
    }

    if (!dev->config.variable_length && dev->config.payload_length == 0) {
        return rfm95_receive_fsk_stream_locked(dev, packet, timeout_ms);
    }

    uint8_t flags = 0;
    ret = rfm95_wait_reg_locked(dev,
                                REG_FSK_IRQ_FLAGS2,
                                FSK_IRQ2_PAYLOAD_READY,
                                false,
                                timeout_ms,
                                &flags);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t rssi = 0;
    uint8_t radio_len =
        (uint8_t)(dev->config.payload_length +
                  (dev->config.has_node_id ? 1U : 0U));
    ret = rfm95_read_reg_locked(dev, REG_FSK_RSSI_VALUE, &rssi);
    if (ret == ESP_OK && dev->config.variable_length) {
        ret = rfm95_read_burst_locked(dev, REG_FIFO, &radio_len, 1);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    const bool has_address = dev->config.has_node_id;
    if (radio_len == 0 ||
        radio_len > RFM95_FSK_MAX_PACKET_LEN + (has_address ? 1U : 0U)) {
        (void)rfm95_write_reg_locked(dev,
                                     REG_FSK_IRQ_FLAGS2,
                                     FSK_IRQ2_FIFO_OVERRUN);
        return ESP_ERR_INVALID_SIZE;
    }

    memset(packet, 0, sizeof(*packet));
    size_t data_len = radio_len;
    if (has_address) {
        uint8_t address = 0;
        ret = rfm95_read_burst_locked(dev, REG_FIFO, &address, 1);
        if (ret != ESP_OK) {
            return ret;
        }
        packet->has_destination = true;
        packet->destination = address;
        data_len--;
    }
    if (data_len > 0) {
        ret = rfm95_read_burst_locked(dev, REG_FIFO, packet->data, data_len);
    }
    if (ret == ESP_OK) {
        packet->len = data_len;
        packet->has_rssi = true;
        packet->rssi_dbm = -(int16_t)(rssi / 2U);
        packet->crc_ok =
            !dev->config.crc_enabled || (flags & FSK_IRQ2_CRC_OK) != 0;
        dev->last_rssi_dbm = packet->rssi_dbm;
        dev->has_last_packet = true;
    }
    return ret;
}

esp_err_t rfm95_receive(rfm95_t *dev,
                        solar_os_radio_packet_t *packet,
                        uint32_t timeout_ms)
{
    if (dev == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = rfm95_lock(dev);
    if (ret == ESP_OK) {
        ret = rfm95_is_lora(&dev->config)
                  ? rfm95_receive_lora_locked(dev, packet, timeout_ms)
                  : rfm95_receive_fsk_locked(dev, packet, timeout_ms);
        rfm95_unlock(dev);
    }
    return ret;
}
