#include "sx1262.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

/*
 * Semtech SX1262 command-set driver. Unlike the SX127x family (see rfm95.c),
 * the SX126x series is not a plain SPI register file: every transaction is a
 * fixed-format command (opcode + parameter bytes [+ response bytes]),
 * gated by the BUSY pin going low before *and* settling high then low again
 * after each command completes. Commands, IRQ bit positions, the LoRa
 * bandwidth/sync-word register quirks, and the TCXO/DIO2-RF-switch
 * configuration below follow the public Semtech SX1261/2 datasheet, cross
 * checked against LilyGO's own T-LoRa-Pager firmware (which calls
 * radio.setTCXO(3.0) and radio.setDio2AsRfSwitch() for this exact module).
 */

#define SX1262_OP_SET_SLEEP 0x84U
#define SX1262_OP_SET_STANDBY 0x80U
#define SX1262_OP_SET_TX 0x83U
#define SX1262_OP_SET_RX 0x82U
#define SX1262_OP_SET_RF_FREQUENCY 0x86U
#define SX1262_OP_SET_PACKET_TYPE 0x8AU
#define SX1262_OP_SET_TX_PARAMS 0x8EU
#define SX1262_OP_SET_MODULATION_PARAMS 0x8BU
#define SX1262_OP_SET_PACKET_PARAMS 0x8CU
#define SX1262_OP_SET_BUFFER_BASE_ADDRESS 0x8FU
#define SX1262_OP_SET_DIO_IRQ_PARAMS 0x08U
#define SX1262_OP_GET_IRQ_STATUS 0x12U
#define SX1262_OP_CLEAR_IRQ_STATUS 0x02U
#define SX1262_OP_CALIBRATE 0x89U
#define SX1262_OP_CALIBRATE_IMAGE 0x98U
#define SX1262_OP_SET_REGULATOR_MODE 0x96U
#define SX1262_OP_GET_STATUS 0xC0U
#define SX1262_OP_GET_RX_BUFFER_STATUS 0x13U
#define SX1262_OP_GET_PACKET_STATUS 0x14U
#define SX1262_OP_WRITE_REGISTER 0x0DU
#define SX1262_OP_WRITE_BUFFER 0x0EU
#define SX1262_OP_READ_BUFFER 0x1EU
#define SX1262_OP_SET_DIO3_AS_TCXO_CTRL 0x97U
#define SX1262_OP_SET_DIO2_AS_RF_SWITCH_CTRL 0x9DU
#define SX1262_OP_SET_STOP_RX_TIMER_ON_PREAMBLE 0x9FU

#define SX1262_STANDBY_RC 0x00U
#define SX1262_REGULATOR_DC_DC 0x01U
#define SX1262_TCXO_VOLTAGE_3V0 0x06U
#define SX1262_TCXO_DELAY_15625NS_UNITS 320U /* ~5 ms startup delay */

#define SX1262_PACKET_TYPE_GFSK 0x00U
#define SX1262_PACKET_TYPE_LORA 0x01U

#define SX1262_IRQ_TX_DONE 0x0001U
#define SX1262_IRQ_RX_DONE 0x0002U
#define SX1262_IRQ_PREAMBLE_DETECTED 0x0004U
#define SX1262_IRQ_SYNC_WORD_VALID 0x0008U
#define SX1262_IRQ_HEADER_VALID 0x0010U
#define SX1262_IRQ_HEADER_ERR 0x0020U
#define SX1262_IRQ_CRC_ERR 0x0040U
#define SX1262_IRQ_TIMEOUT 0x0200U
#define SX1262_IRQ_ALL 0xFFFFU

#define SX1262_REG_LORA_SYNC_WORD_MSB 0x0740U
#define SX1262_REG_LORA_SYNC_WORD_LSB 0x0741U

#define SX1262_BUSY_TIMEOUT_US 1000000ULL
#define SX1262_POLL_INTERVAL_MS 4U
#define SX1262_FXTAL_HZ 32000000ULL

static esp_err_t sx1262_lock(sx1262_t *dev)
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

static void sx1262_unlock(sx1262_t *dev)
{
    if (dev != NULL && dev->mutex != NULL) {
        xSemaphoreGive(dev->mutex);
    }
}

static esp_err_t sx1262_wait_busy(sx1262_t *dev)
{
    const int64_t start = esp_timer_get_time();
    while (gpio_get_level(dev->busy_pin) != 0) {
        if ((uint64_t)(esp_timer_get_time() - start) > SX1262_BUSY_TIMEOUT_US) {
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(5);
    }
    return ESP_OK;
}

/* One full command transaction: opcode + params out, with `resp_len` extra
 * clock cycles appended to fetch a response (0 for pure "set" commands).
 * `resp` receives the response bytes, if any, with the always-present
 * leading status byte already stripped. */
static esp_err_t sx1262_transact(sx1262_t *dev,
                                 uint8_t opcode,
                                 const uint8_t *params,
                                 size_t param_len,
                                 uint8_t *resp,
                                 size_t resp_len)
{
    uint8_t tx[3 + SX1262_MAX_PACKET_LEN];
    uint8_t rx[3 + SX1262_MAX_PACKET_LEN];
    const size_t total = 1 + param_len + resp_len;

    if (total > sizeof(tx) || (resp_len > 0 && resp == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sx1262_wait_busy(dev);
    if (err != ESP_OK) {
        return err;
    }
    memset(tx, 0, total);
    tx[0] = opcode;
    if (param_len > 0) {
        memcpy(&tx[1], params, param_len);
    }
    err = solar_os_bus_spi_transfer(dev->spi_bus, dev->cs_pin, 0, dev->speed_hz, tx, rx, total);
    if (err != ESP_OK) {
        return err;
    }
    if (resp_len > 0) {
        memcpy(resp, &rx[1 + param_len], resp_len);
    }
    return ESP_OK;
}

static esp_err_t sx1262_write_register(sx1262_t *dev, uint16_t addr, const uint8_t *data, size_t len)
{
    uint8_t params[2 + 16];
    if (len > sizeof(params) - 2) {
        return ESP_ERR_INVALID_ARG;
    }
    params[0] = (uint8_t)(addr >> 8);
    params[1] = (uint8_t)(addr & 0xFFU);
    memcpy(&params[2], data, len);
    return sx1262_transact(dev, SX1262_OP_WRITE_REGISTER, params, 2 + len, NULL, 0);
}

static esp_err_t sx1262_write_buffer(sx1262_t *dev, uint8_t offset, const uint8_t *data, size_t len)
{
    uint8_t params[1 + SX1262_MAX_PACKET_LEN];
    if (len > SX1262_MAX_PACKET_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    params[0] = offset;
    memcpy(&params[1], data, len);
    return sx1262_transact(dev, SX1262_OP_WRITE_BUFFER, params, 1 + len, NULL, 0);
}

static esp_err_t sx1262_read_buffer(sx1262_t *dev, uint8_t offset, uint8_t *data, size_t len)
{
    /* ReadBuffer clocks: opcode, offset, one status/NOP byte, then `len`
     * data bytes - the status byte is part of the "response" window here,
     * not the generic single leading status byte the other commands use. */
    uint8_t tx[3 + SX1262_MAX_PACKET_LEN];
    uint8_t rx[3 + SX1262_MAX_PACKET_LEN];
    const size_t total = 2 + 1 + len;

    if (len > SX1262_MAX_PACKET_LEN || total > sizeof(tx)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sx1262_wait_busy(dev);
    if (err != ESP_OK) {
        return err;
    }
    memset(tx, 0, total);
    tx[0] = SX1262_OP_READ_BUFFER;
    tx[1] = offset;
    err = solar_os_bus_spi_transfer(dev->spi_bus, dev->cs_pin, 0, dev->speed_hz, tx, rx, total);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(data, &rx[3], len);
    return ESP_OK;
}

static esp_err_t sx1262_set_standby(sx1262_t *dev)
{
    const uint8_t param = SX1262_STANDBY_RC;
    return sx1262_transact(dev, SX1262_OP_SET_STANDBY, &param, 1, NULL, 0);
}

static esp_err_t sx1262_get_irq_status(sx1262_t *dev, uint16_t *irq)
{
    /* Response window is [status, irqMSB, irqLSB] - the status byte is not
     * a leading artifact stripped by sx1262_transact() the way it is for
     * single-byte GetStatus; here it occupies the first response slot and
     * must be skipped explicitly. */
    uint8_t resp[3] = {0};
    const esp_err_t err = sx1262_transact(dev, SX1262_OP_GET_IRQ_STATUS, NULL, 0, resp, 3);
    if (err == ESP_OK && irq != NULL) {
        *irq = (uint16_t)((resp[1] << 8) | resp[2]);
    }
    return err;
}

static esp_err_t sx1262_clear_irq_status(sx1262_t *dev, uint16_t mask)
{
    const uint8_t params[2] = {(uint8_t)(mask >> 8), (uint8_t)(mask & 0xFFU)};
    return sx1262_transact(dev, SX1262_OP_CLEAR_IRQ_STATUS, params, 2, NULL, 0);
}

static esp_err_t sx1262_set_dio_irq_params(sx1262_t *dev, uint16_t irq_mask, uint16_t dio1_mask)
{
    const uint8_t params[8] = {
        (uint8_t)(irq_mask >> 8), (uint8_t)(irq_mask & 0xFFU),
        (uint8_t)(dio1_mask >> 8), (uint8_t)(dio1_mask & 0xFFU),
        0x00U, 0x00U,
        0x00U, 0x00U,
    };
    return sx1262_transact(dev, SX1262_OP_SET_DIO_IRQ_PARAMS, params, sizeof(params), NULL, 0);
}

esp_err_t sx1262_init(sx1262_t *dev,
                      const char *spi_bus,
                      int cs_pin,
                      int busy_pin,
                      int reset_pin,
                      int irq_pin,
                      uint32_t speed_hz)
{
    if (dev == NULL || spi_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(dev, 0, sizeof(*dev));
    strlcpy(dev->spi_bus, spi_bus, sizeof(dev->spi_bus));
    dev->cs_pin = cs_pin;
    dev->busy_pin = busy_pin;
    dev->reset_pin = reset_pin;
    dev->irq_pin = irq_pin;
    dev->speed_hz = speed_hz;
    dev->state = SOLAR_OS_RADIO_STATE_UNKNOWN;

    const gpio_config_t busy_config = {
        .pin_bit_mask = 1ULL << (uint32_t)busy_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&busy_config);
    if (err != ESP_OK) {
        return err;
    }

    if (irq_pin >= 0) {
        const gpio_config_t irq_config = {
            .pin_bit_mask = 1ULL << (uint32_t)irq_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&irq_config);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (reset_pin >= 0) {
        const gpio_config_t reset_config = {
            .pin_bit_mask = 1ULL << (uint32_t)reset_pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&reset_config);
        if (err != ESP_OK) {
            return err;
        }
        gpio_set_level(reset_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
        gpio_set_level(reset_pin, 0);
        esp_rom_delay_us(200);
        gpio_set_level(reset_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    return sx1262_wait_busy(dev);
}

esp_err_t sx1262_probe(sx1262_t *dev, uint8_t *chip_mode)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t status = 0U;
    /* GetStatus response is a single status byte; bits [6:4] are chip mode
     * (2 = STBY_RC after reset, among other valid values). Any successful,
     * non-garbage transfer here is treated as "chip present" - a genuinely
     * absent/dead module tends to read back 0x00 or 0xFF continuously. */
    const esp_err_t err = sx1262_transact(dev, SX1262_OP_GET_STATUS, NULL, 0, &status, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (status == 0x00U || status == 0xFFU) {
        return ESP_ERR_NOT_FOUND;
    }
    if (chip_mode != NULL) {
        *chip_mode = status;
    }
    return ESP_OK;
}

static uint8_t sx1262_lora_bandwidth_code(uint32_t bw_hz)
{
    if (bw_hz <= 7800U) return 0x00U;
    if (bw_hz <= 10400U) return 0x08U;
    if (bw_hz <= 15600U) return 0x01U;
    if (bw_hz <= 20800U) return 0x09U;
    if (bw_hz <= 31250U) return 0x02U;
    if (bw_hz <= 41700U) return 0x0AU;
    if (bw_hz <= 62500U) return 0x03U;
    if (bw_hz <= 125000U) return 0x04U;
    if (bw_hz <= 250000U) return 0x05U;
    return 0x06U; /* 500 kHz */
}

static void sx1262_image_calibration_bytes(uint32_t freq_hz, uint8_t *freq1, uint8_t *freq2)
{
    if (freq_hz >= 902000000U && freq_hz <= 928000000U) {
        *freq1 = 0xE1U; *freq2 = 0xE9U; /* US915 / AU915 */
    } else if (freq_hz >= 863000000U && freq_hz <= 870000000U) {
        *freq1 = 0xD7U; *freq2 = 0xDBU; /* EU868 */
    } else if (freq_hz >= 779000000U && freq_hz <= 787000000U) {
        *freq1 = 0xC1U; *freq2 = 0xC5U; /* CN779 */
    } else if (freq_hz >= 470000000U && freq_hz <= 510000000U) {
        *freq1 = 0x75U; *freq2 = 0x81U; /* CN470 */
    } else if (freq_hz >= 430000000U && freq_hz <= 440000000U) {
        *freq1 = 0x6BU; *freq2 = 0x6FU; /* 433 MHz ISM */
    } else {
        /* Fall back to the widest documented band; calibration will be
         * less accurate outside a known plan but should not fail outright. */
        *freq1 = 0xE1U; *freq2 = 0xE9U;
    }
}

static esp_err_t sx1262_set_lora_sync_word(sx1262_t *dev, uint8_t sync_word)
{
    /* SX126x stores the LoRa sync word split across two nibble-mapped
     * registers rather than as a plain byte. 0x34 selects the LoRaWAN
     * "public" mapping; anything else (0x12 by convention, used by the
     * built-in meshcore/lora profiles here) selects the "private" mapping. */
    uint8_t msb, lsb;
    if (sync_word == 0x34U) {
        msb = 0x34U; lsb = 0x44U;
    } else {
        msb = 0x14U; lsb = 0x24U;
    }
    esp_err_t err = sx1262_write_register(dev, SX1262_REG_LORA_SYNC_WORD_MSB, &msb, 1);
    if (err != ESP_OK) {
        return err;
    }
    return sx1262_write_register(dev, SX1262_REG_LORA_SYNC_WORD_LSB, &lsb, 1);
}

static bool sx1262_is_lora(const solar_os_radio_config_t *config)
{
    return config != NULL && config->modulation == SOLAR_OS_RADIO_MODULATION_LORA;
}

esp_err_t sx1262_configure(sx1262_t *dev, const solar_os_radio_config_t *config)
{
    if (dev == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sx1262_lock(dev);
    if (err != ESP_OK) {
        return err;
    }

    const bool lora = sx1262_is_lora(config);

    err = sx1262_set_standby(dev);
    if (err == ESP_OK) {
        const uint8_t regulator = SX1262_REGULATOR_DC_DC;
        err = sx1262_transact(dev, SX1262_OP_SET_REGULATOR_MODE, &regulator, 1, NULL, 0);
    }
    if (err == ESP_OK) {
        const uint8_t tcxo_params[4] = {
            SX1262_TCXO_VOLTAGE_3V0,
            (uint8_t)(SX1262_TCXO_DELAY_15625NS_UNITS >> 16),
            (uint8_t)(SX1262_TCXO_DELAY_15625NS_UNITS >> 8),
            (uint8_t)(SX1262_TCXO_DELAY_15625NS_UNITS & 0xFFU),
        };
        err = sx1262_transact(dev, SX1262_OP_SET_DIO3_AS_TCXO_CTRL, tcxo_params, sizeof(tcxo_params), NULL, 0);
    }
    if (err == ESP_OK) {
        const uint8_t calib_param = 0x7FU;
        err = sx1262_transact(dev, SX1262_OP_CALIBRATE, &calib_param, 1, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (err == ESP_OK) {
        uint8_t freq1 = 0U, freq2 = 0U;
        sx1262_image_calibration_bytes(config->frequency_hz, &freq1, &freq2);
        const uint8_t params[2] = {freq1, freq2};
        err = sx1262_transact(dev, SX1262_OP_CALIBRATE_IMAGE, params, sizeof(params), NULL, 0);
    }
    if (err == ESP_OK) {
        const uint32_t freq_reg =
            (uint32_t)((((uint64_t)config->frequency_hz << 25) + (SX1262_FXTAL_HZ / 2)) / SX1262_FXTAL_HZ);
        const uint8_t params[4] = {
            (uint8_t)(freq_reg >> 24), (uint8_t)(freq_reg >> 16),
            (uint8_t)(freq_reg >> 8), (uint8_t)(freq_reg & 0xFFU),
        };
        err = sx1262_transact(dev, SX1262_OP_SET_RF_FREQUENCY, params, sizeof(params), NULL, 0);
    }
    if (err == ESP_OK) {
        const uint8_t packet_type = lora ? SX1262_PACKET_TYPE_LORA : SX1262_PACKET_TYPE_GFSK;
        err = sx1262_transact(dev, SX1262_OP_SET_PACKET_TYPE, &packet_type, 1, NULL, 0);
    }
    if (err == ESP_OK) {
        if (lora) {
            const uint8_t sf = (config->spreading_factor >= 5 && config->spreading_factor <= 12)
                ? config->spreading_factor : 7U;
            const uint8_t bw = sx1262_lora_bandwidth_code(
                config->rx_bandwidth_hz != 0 ? config->rx_bandwidth_hz : 125000U);
            const uint8_t cr = (config->coding_rate_denominator >= 5 && config->coding_rate_denominator <= 8)
                ? (uint8_t)(config->coding_rate_denominator - 4U) : 1U;
            const uint8_t low_dr_opt = (bw == 0x00U || bw == 0x08U ||
                                        (bw == 0x01U && sf >= 11U) ||
                                        (bw == 0x02U && sf == 12U)) ? 1U : 0U;
            const uint8_t params[4] = {sf, bw, cr, low_dr_opt};
            err = sx1262_transact(dev, SX1262_OP_SET_MODULATION_PARAMS, params, sizeof(params), NULL, 0);
        } else {
            const uint32_t bitrate_hz = config->bitrate_bps != 0 ? config->bitrate_bps : 4800U;
            const uint32_t br_reg = (uint32_t)((32ULL * SX1262_FXTAL_HZ) / bitrate_hz);
            const uint32_t fdev_hz = config->deviation_hz != 0 ? config->deviation_hz : 5000U;
            const uint32_t fdev_reg =
                (uint32_t)(((uint64_t)fdev_hz << 25) / SX1262_FXTAL_HZ);
            const uint8_t params[8] = {
                (uint8_t)(br_reg >> 16), (uint8_t)(br_reg >> 8), (uint8_t)(br_reg & 0xFFU),
                0x00U, /* no pulse shaping */
                0x0FU, /* ~234.3 kHz Rx bandwidth: safe wideband default */
                (uint8_t)(fdev_reg >> 16), (uint8_t)(fdev_reg >> 8), (uint8_t)(fdev_reg & 0xFFU),
            };
            err = sx1262_transact(dev, SX1262_OP_SET_MODULATION_PARAMS, params, sizeof(params), NULL, 0);
        }
    }
    if (err == ESP_OK) {
        const uint16_t preamble = config->preamble_len != 0 ? config->preamble_len : 8U;
        const uint8_t payload_len = (uint8_t)(config->payload_length != 0 ? config->payload_length : 1U);
        if (lora) {
            const uint8_t params[6] = {
                (uint8_t)(preamble >> 8), (uint8_t)(preamble & 0xFFU),
                config->variable_length ? 0x00U : 0x01U,
                payload_len,
                config->crc_enabled ? 0x01U : 0x00U,
                0x00U, /* standard IQ */
            };
            err = sx1262_transact(dev, SX1262_OP_SET_PACKET_PARAMS, params, sizeof(params), NULL, 0);
        } else {
            const uint16_t preamble_bits = (uint16_t)(preamble * 8U);
            const uint8_t sync_len_bits = (uint8_t)((config->sync_word_len != 0 ? config->sync_word_len : 1U) * 8U);
            const uint8_t params[9] = {
                (uint8_t)(preamble_bits >> 8), (uint8_t)(preamble_bits & 0xFFU),
                0x04U, /* 8-bit preamble detector */
                sync_len_bits,
                0x00U, /* no address filtering */
                config->variable_length ? 0x01U : 0x00U,
                payload_len,
                config->crc_enabled ? 0x02U : 0x00U, /* 2-byte CRC when enabled */
                0x00U, /* no whitening */
            };
            err = sx1262_transact(dev, SX1262_OP_SET_PACKET_PARAMS, params, sizeof(params), NULL, 0);
        }
    }
    if (err == ESP_OK) {
        const uint8_t params[2] = {0x00U, 0x00U};
        err = sx1262_transact(dev, SX1262_OP_SET_BUFFER_BASE_ADDRESS, params, sizeof(params), NULL, 0);
    }
    if (err == ESP_OK) {
        int8_t power = (int8_t)config->tx_power_dbm;
        if (power > 22) power = 22;
        if (power < -17) power = -17;
        const uint8_t params[2] = {(uint8_t)power, 0x04U /* 200 us ramp */};
        err = sx1262_transact(dev, SX1262_OP_SET_TX_PARAMS, params, sizeof(params), NULL, 0);
    }
    if (err == ESP_OK) {
        const uint8_t enable = 0x01U;
        err = sx1262_transact(dev, SX1262_OP_SET_DIO2_AS_RF_SWITCH_CTRL, &enable, 1, NULL, 0);
    }
    if (err == ESP_OK) {
        const uint16_t irq_mask = SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE | SX1262_IRQ_TIMEOUT |
            SX1262_IRQ_CRC_ERR | SX1262_IRQ_HEADER_ERR;
        err = sx1262_set_dio_irq_params(dev, irq_mask, irq_mask);
    }
    if (err == ESP_OK && lora) {
        err = sx1262_set_lora_sync_word(dev, config->sync_word_len != 0 ? config->sync_word[0] : 0x12U);
    }
    if (err == ESP_OK) {
        err = sx1262_clear_irq_status(dev, SX1262_IRQ_ALL);
    }
    if (err == ESP_OK) {
        dev->config = *config;
        dev->state = SOLAR_OS_RADIO_STATE_STANDBY;
    }

    sx1262_unlock(dev);
    return err;
}

esp_err_t sx1262_set_state(sx1262_t *dev, solar_os_radio_state_t state)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sx1262_lock(dev);
    if (err != ESP_OK) {
        return err;
    }

    switch (state) {
    case SOLAR_OS_RADIO_STATE_SLEEP: {
        const uint8_t param = 0x00U; /* cold start, no RTC wakeup */
        err = sx1262_transact(dev, SX1262_OP_SET_SLEEP, &param, 1, NULL, 0);
        break;
    }
    case SOLAR_OS_RADIO_STATE_STANDBY:
        err = sx1262_set_standby(dev);
        break;
    case SOLAR_OS_RADIO_STATE_RX: {
        const uint8_t params[3] = {0xFFU, 0xFFU, 0xFFU}; /* continuous */
        err = sx1262_transact(dev, SX1262_OP_SET_RX, params, sizeof(params), NULL, 0);
        break;
    }
    case SOLAR_OS_RADIO_STATE_TX: {
        const uint8_t params[3] = {0x00U, 0x00U, 0x00U}; /* no HW timeout */
        err = sx1262_transact(dev, SX1262_OP_SET_TX, params, sizeof(params), NULL, 0);
        break;
    }
    case SOLAR_OS_RADIO_STATE_UNKNOWN:
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }
    if (err == ESP_OK) {
        dev->state = state;
    }

    sx1262_unlock(dev);
    return err;
}

esp_err_t sx1262_get_status(sx1262_t *dev, solar_os_radio_status_t *status)
{
    if (dev == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sx1262_lock(dev);
    if (err != ESP_OK) {
        return err;
    }
    memset(status, 0, sizeof(*status));
    status->state = dev->state;
    status->config = dev->config;
    if (dev->has_last_packet) {
        status->has_rssi = true;
        status->rssi_dbm = dev->last_rssi_dbm;
        status->has_snr = true;
        status->snr_db = dev->last_snr_db;
    }
    sx1262_unlock(dev);
    return ESP_OK;
}

esp_err_t sx1262_send(sx1262_t *dev, const solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    if (dev == NULL || packet == NULL || packet->len == 0 || packet->len > SX1262_MAX_PACKET_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sx1262_lock(dev);
    if (err != ESP_OK) {
        return err;
    }

    const bool lora = sx1262_is_lora(&dev->config);
    const uint8_t payload_len = (uint8_t)packet->len;
    if (err == ESP_OK) {
        if (lora) {
            const uint8_t params[6] = {
                (uint8_t)(dev->config.preamble_len >> 8), (uint8_t)(dev->config.preamble_len & 0xFFU),
                dev->config.variable_length ? 0x00U : 0x01U,
                payload_len,
                dev->config.crc_enabled ? 0x01U : 0x00U,
                0x00U,
            };
            err = sx1262_transact(dev, SX1262_OP_SET_PACKET_PARAMS, params, sizeof(params), NULL, 0);
        } else {
            const uint16_t preamble_bits = (uint16_t)(dev->config.preamble_len * 8U);
            const uint8_t sync_len_bits = (uint8_t)((dev->config.sync_word_len != 0 ? dev->config.sync_word_len : 1U) * 8U);
            const uint8_t params[9] = {
                (uint8_t)(preamble_bits >> 8), (uint8_t)(preamble_bits & 0xFFU),
                0x04U, sync_len_bits, 0x00U,
                dev->config.variable_length ? 0x01U : 0x00U,
                payload_len,
                dev->config.crc_enabled ? 0x02U : 0x00U,
                0x00U,
            };
            err = sx1262_transact(dev, SX1262_OP_SET_PACKET_PARAMS, params, sizeof(params), NULL, 0);
        }
    }
    if (err == ESP_OK) {
        err = sx1262_clear_irq_status(dev, SX1262_IRQ_ALL);
    }
    if (err == ESP_OK) {
        err = sx1262_write_buffer(dev, 0x00U, packet->data, packet->len);
    }
    if (err == ESP_OK) {
        const uint8_t params[3] = {0x00U, 0x00U, 0x00U};
        err = sx1262_transact(dev, SX1262_OP_SET_TX, params, sizeof(params), NULL, 0);
    }
    if (err == ESP_OK) {
        dev->state = SOLAR_OS_RADIO_STATE_TX;
        const int64_t start = esp_timer_get_time();
        bool done = false;
        while (!done) {
            uint16_t irq = 0U;
            err = sx1262_get_irq_status(dev, &irq);
            if (err != ESP_OK) {
                break;
            }
            if (irq & SX1262_IRQ_TX_DONE) {
                (void)sx1262_clear_irq_status(dev, SX1262_IRQ_ALL);
                done = true;
                break;
            }
            if (irq & SX1262_IRQ_TIMEOUT) {
                (void)sx1262_clear_irq_status(dev, SX1262_IRQ_ALL);
                err = ESP_ERR_TIMEOUT;
                break;
            }
            if ((uint32_t)((esp_timer_get_time() - start) / 1000) >= timeout_ms) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(SX1262_POLL_INTERVAL_MS));
        }
        (void)sx1262_set_standby(dev);
        dev->state = SOLAR_OS_RADIO_STATE_STANDBY;
    }

    sx1262_unlock(dev);
    return err;
}

esp_err_t sx1262_send_stream(sx1262_t *dev, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (data == NULL || len == 0 || len > SX1262_MAX_PACKET_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_radio_packet_t packet = {0};
    packet.len = len;
    memcpy(packet.data, data, len);
    return sx1262_send(dev, &packet, timeout_ms);
}

esp_err_t sx1262_receive(sx1262_t *dev, solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    if (dev == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sx1262_lock(dev);
    if (err != ESP_OK) {
        return err;
    }
    memset(packet, 0, sizeof(*packet));

    err = sx1262_clear_irq_status(dev, SX1262_IRQ_ALL);
    if (err == ESP_OK) {
        const uint8_t params[3] = {0xFFU, 0xFFU, 0xFFU}; /* continuous; SW owns the timeout */
        err = sx1262_transact(dev, SX1262_OP_SET_RX, params, sizeof(params), NULL, 0);
    }
    if (err != ESP_OK) {
        sx1262_unlock(dev);
        return err;
    }

    dev->state = SOLAR_OS_RADIO_STATE_RX;
    const int64_t start = esp_timer_get_time();
    bool rx_done = false;
    while (true) {
        uint16_t irq = 0U;
        err = sx1262_get_irq_status(dev, &irq);
        if (err != ESP_OK) {
            break;
        }
        if (irq & SX1262_IRQ_RX_DONE) {
            rx_done = true;
            packet->crc_ok = (irq & SX1262_IRQ_CRC_ERR) == 0U;
            (void)sx1262_clear_irq_status(dev, SX1262_IRQ_ALL);
            break;
        }
        if ((uint32_t)((esp_timer_get_time() - start) / 1000) >= timeout_ms) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(SX1262_POLL_INTERVAL_MS));
    }
    (void)sx1262_set_standby(dev);
    dev->state = SOLAR_OS_RADIO_STATE_STANDBY;

    if (rx_done && err == ESP_OK) {
        /* Both "Get" responses below carry a leading status byte before the
         * real data, same as GetIrqStatus - see sx1262_get_irq_status(). */
        uint8_t status_resp[3] = {0};
        err = sx1262_transact(dev, SX1262_OP_GET_RX_BUFFER_STATUS, NULL, 0, status_resp, 3);
        size_t payload_len = 0;
        uint8_t start_ptr = 0;
        if (err == ESP_OK) {
            payload_len = status_resp[1];
            start_ptr = status_resp[2];
            if (payload_len > SOLAR_OS_RADIO_PACKET_MAX) {
                payload_len = SOLAR_OS_RADIO_PACKET_MAX;
            }
            err = sx1262_read_buffer(dev, start_ptr, packet->data, payload_len);
        }
        if (err == ESP_OK) {
            packet->len = payload_len;
            uint8_t pkt_status[4] = {0};
            if (sx1262_transact(dev, SX1262_OP_GET_PACKET_STATUS, NULL, 0, pkt_status, 4) == ESP_OK) {
                if (sx1262_is_lora(&dev->config)) {
                    packet->has_rssi = true;
                    packet->rssi_dbm = (int16_t)(-((int16_t)pkt_status[1]) / 2);
                    packet->has_snr = true;
                    packet->snr_db = (int16_t)(((int8_t)pkt_status[2]) / 4);
                    dev->last_rssi_dbm = packet->rssi_dbm;
                    dev->last_snr_db = packet->snr_db;
                    dev->has_last_packet = true;
                } else {
                    packet->has_rssi = true;
                    packet->rssi_dbm = (int16_t)(-((int16_t)pkt_status[1]) / 2);
                    dev->last_rssi_dbm = packet->rssi_dbm;
                    dev->has_last_packet = true;
                }
            }
        }
    }

    sx1262_unlock(dev);
    return err;
}
