#include "st25r3916.h"

#include <string.h>

#define SPI_READ 0x40U
#define SPI_FIFO_WRITE 0x80U
#define SPI_FIFO_READ 0x9FU
#define CMD_SET_DEFAULT 0xC1U
#define CMD_STOP 0xC2U
#define CMD_TRANSMIT_WITH_CRC 0xC4U
#define CMD_TRANSMIT_WITHOUT_CRC 0xC5U
#define CMD_TRANSMIT_REQA 0xC6U
#define CMD_ADJUST_REGULATORS 0xD6U
#define CMD_CLEAR_FIFO 0xDBU
#define CMD_SPACE_B_ACCESS 0xFBU

#define REG_IO_CONF1 0x00U
#define REG_IO_CONF2 0x01U
#define REG_OP_CONTROL 0x02U
#define REG_MODE 0x03U
#define REG_BIT_RATE 0x04U
#define REG_ISO14443A_NFC 0x05U
#define REG_AUX 0x0AU
#define REG_IRQ_MASK_MAIN 0x16U
#define REG_IRQ_MASK_TIMER_NFC 0x17U
#define REG_IRQ_MASK_ERROR_WUP 0x18U
#define REG_IRQ_MASK_TARGET 0x19U
#define REG_IRQ_MAIN 0x1AU
#define REG_FIFO_STATUS1 0x1EU
#define REG_FIFO_STATUS2 0x1FU
#define REG_NUM_TX_BYTES1 0x22U
#define REG_NUM_TX_BYTES2 0x23U
#define REG_ANT_TUNE_A 0x26U
#define REG_ANT_TUNE_B 0x27U
#define REG_TX_DRIVER 0x28U
#define REG_AUX_DISPLAY 0x31U
#define REG_IC_IDENTITY 0x3FU
#define REG_B_EMD_SUP_CONF 0x45U
#define REG_B_PASSIVE_TARGET 0x48U
#define REG_B_CORR_CONF1 0x4CU
#define REG_B_CORR_CONF2 0x4DU
#define REG_B_AUX_MOD 0x68U
#define REG_B_PT_MOD 0x69U
#define REG_B_RES_AM_MOD 0x6AU
#define REG_B_AWS_CONF1 0x6EU
#define REG_B_AWS_CONF2 0x6FU
#define REG_B_OVERSHOOT_CONF1 0x70U
#define REG_B_OVERSHOOT_CONF2 0x71U
#define REG_B_UNDERSHOOT_CONF1 0x72U
#define REG_B_UNDERSHOOT_CONF2 0x73U
#define REG_B_AWS_TIME1 0x74U
#define REG_B_AWS_TIME3 0x76U
#define REG_B_AWS_TIME4 0x77U

#define OP_CONTROL_EN 0x80U
#define OP_CONTROL_RX_EN 0x40U
#define OP_CONTROL_TX_EN 0x08U
#define AUX_NO_CRC_RX 0x80U
#define AUX_DISPLAY_OSC_OK 0x10U
#define ISO14443A_ANTICOLLISION 0x01U
#define IC_TYPE_MASK 0xF8U
#define IC_TYPE_ST25R3916 0x28U
#define IC_TYPE_ST25R3916B 0x30U

#define IRQ_RXE 0x000010U
#define IRQ_TXE 0x000008U
#define IRQ_COLLISION 0x000004U
#define IRQ_CRC_ERROR 0x800000U
#define IRQ_PARITY_ERROR 0x400000U
#define IRQ_SOFT_FRAMING_ERROR 0x200000U
#define IRQ_HARD_FRAMING_ERROR 0x100000U
#define IRQ_ERROR_MASK (IRQ_COLLISION | IRQ_CRC_ERROR | IRQ_PARITY_ERROR | \
                        IRQ_SOFT_FRAMING_ERROR | IRQ_HARD_FRAMING_ERROR)

typedef struct {
    uint8_t reg;
    uint8_t value;
} register_config_t;

static esp_err_t transfer(st25r3916_t *device,
                          const uint8_t *tx,
                          uint8_t *rx,
                          size_t len)
{
    return device->io.transfer(device->io.ctx, tx, rx, len);
}

static esp_err_t command(st25r3916_t *device, uint8_t value)
{
    uint8_t rx = 0U;
    return transfer(device, &value, &rx, 1U);
}

static esp_err_t reg_read(st25r3916_t *device, uint8_t reg, uint8_t *value)
{
    uint8_t tx[3] = {0};
    uint8_t rx[3] = {0};
    size_t offset = 0U;
    if ((reg & 0x40U) != 0U) {
        tx[offset++] = CMD_SPACE_B_ACCESS;
    }
    tx[offset++] = (uint8_t)(SPI_READ | (reg & 0x3FU));
    const esp_err_t ret = transfer(device, tx, rx, offset + 1U);
    if (ret == ESP_OK) {
        *value = rx[offset];
    }
    return ret;
}

static esp_err_t reg_write(st25r3916_t *device, uint8_t reg, uint8_t value)
{
    uint8_t tx[3] = {0};
    uint8_t rx[3] = {0};
    size_t offset = 0U;
    if ((reg & 0x40U) != 0U) {
        tx[offset++] = CMD_SPACE_B_ACCESS;
    }
    tx[offset++] = reg & 0x3FU;
    tx[offset++] = value;
    return transfer(device, tx, rx, offset);
}

static esp_err_t reg_change(st25r3916_t *device,
                            uint8_t reg,
                            uint8_t mask,
                            uint8_t value)
{
    uint8_t current = 0U;
    esp_err_t ret = reg_read(device, reg, &current);
    if (ret == ESP_OK) {
        ret = reg_write(device, reg,
                        (uint8_t)((current & (uint8_t)~mask) | (value & mask)));
    }
    return ret;
}

static esp_err_t fifo_write(st25r3916_t *device, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U || len > 32U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tx[33] = {SPI_FIFO_WRITE};
    uint8_t rx[33] = {0};
    memcpy(&tx[1], data, len);
    return transfer(device, tx, rx, len + 1U);
}

static esp_err_t fifo_read(st25r3916_t *device, uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U || len > 32U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tx[33] = {SPI_FIFO_READ};
    uint8_t rx[33] = {0};
    esp_err_t ret = transfer(device, tx, rx, len + 1U);
    if (ret == ESP_OK) {
        memcpy(data, &rx[1], len);
    }
    return ret;
}

static esp_err_t clear_irqs(st25r3916_t *device)
{
    uint8_t value = 0U;
    for (uint8_t reg = REG_IRQ_MAIN; reg < REG_IRQ_MAIN + 4U; reg++) {
        const esp_err_t ret = reg_read(device, reg, &value);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t read_irqs(st25r3916_t *device, uint32_t *irqs)
{
    *irqs = 0U;
    for (uint8_t i = 0U; i < 3U; i++) {
        uint8_t value = 0U;
        const esp_err_t ret = reg_read(device, (uint8_t)(REG_IRQ_MAIN + i), &value);
        if (ret != ESP_OK) {
            return ret;
        }
        *irqs |= (uint32_t)value << (i * 8U);
    }
    return ESP_OK;
}

static esp_err_t wait_receive(st25r3916_t *device, uint32_t timeout_ms)
{
    const int64_t deadline = device->io.time_ms(device->io.ctx) + timeout_ms;
    while (device->io.time_ms(device->io.ctx) < deadline) {
        if (device->io.irq_asserted == NULL ||
            device->io.irq_asserted(device->io.ctx)) {
            uint32_t irqs = 0U;
            const esp_err_t ret = read_irqs(device, &irqs);
            if (ret != ESP_OK) {
                return ret;
            }
            if ((irqs & IRQ_ERROR_MASK) != 0U) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            if ((irqs & IRQ_RXE) != 0U) {
                return ESP_OK;
            }
        }
        device->io.delay_ms(device->io.ctx, 1U);
    }
    (void)command(device, CMD_STOP);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t receive_fifo(st25r3916_t *device,
                              uint8_t *data,
                              size_t capacity,
                              size_t *received)
{
    uint8_t status1 = 0U;
    uint8_t status2 = 0U;
    esp_err_t ret = reg_read(device, REG_FIFO_STATUS1, &status1);
    if (ret == ESP_OK) {
        ret = reg_read(device, REG_FIFO_STATUS2, &status2);
    }
    const size_t len = (size_t)status1 + (((size_t)status2 & 0xC0U) << 2U);
    if (ret != ESP_OK) {
        return ret;
    }
    if (len == 0U || len > capacity || (status2 & 0x3EU) != 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    ret = fifo_read(device, data, len);
    if (ret == ESP_OK) {
        *received = len;
    }
    return ret;
}

static esp_err_t transceive(st25r3916_t *device,
                            const uint8_t *tx,
                            size_t tx_len,
                            bool append_crc,
                            bool check_crc,
                            uint8_t *rx,
                            size_t rx_capacity,
                            size_t *rx_len,
                            uint32_t timeout_ms)
{
    esp_err_t ret = command(device, CMD_CLEAR_FIFO);
    if (ret == ESP_OK) {
        ret = clear_irqs(device);
    }
    if (ret == ESP_OK) {
        ret = reg_change(device, REG_AUX, AUX_NO_CRC_RX,
                         check_crc ? 0U : AUX_NO_CRC_RX);
    }
    if (ret == ESP_OK) {
        ret = fifo_write(device, tx, tx_len);
    }
    const uint16_t bits = (uint16_t)(tx_len * 8U);
    if (ret == ESP_OK) {
        ret = reg_write(device, REG_NUM_TX_BYTES2, (uint8_t)bits);
    }
    if (ret == ESP_OK) {
        ret = reg_write(device, REG_NUM_TX_BYTES1, (uint8_t)(bits >> 8U));
    }
    if (ret == ESP_OK) {
        ret = command(device,
                      append_crc ? CMD_TRANSMIT_WITH_CRC : CMD_TRANSMIT_WITHOUT_CRC);
    }
    if (ret == ESP_OK) {
        ret = wait_receive(device, timeout_ms);
    }
    if (ret == ESP_OK) {
        ret = receive_fifo(device, rx, rx_capacity, rx_len);
    }
    return ret;
}

static esp_err_t reqa(st25r3916_t *device, uint8_t atqa[2], uint32_t timeout_ms)
{
    esp_err_t ret = command(device, CMD_CLEAR_FIFO);
    if (ret == ESP_OK) {
        ret = clear_irqs(device);
    }
    if (ret == ESP_OK) {
        ret = reg_change(device, REG_AUX, AUX_NO_CRC_RX, AUX_NO_CRC_RX);
    }
    if (ret == ESP_OK) {
        ret = reg_write(device, REG_NUM_TX_BYTES2, 0U);
    }
    if (ret == ESP_OK) {
        ret = command(device, CMD_TRANSMIT_REQA);
    }
    if (ret == ESP_OK) {
        ret = wait_receive(device, timeout_ms);
    }
    size_t received = 0U;
    if (ret == ESP_OK) {
        ret = receive_fifo(device, atqa, 2U, &received);
    }
    return ret == ESP_OK && received == 2U ? ESP_OK :
        (ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret);
}

esp_err_t st25r3916_init(st25r3916_t *device, const st25r3916_io_t *io)
{
    if (device == NULL || io == NULL || io->transfer == NULL ||
        io->delay_ms == NULL || io->time_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    device->io = *io;
    esp_err_t ret = command(device, CMD_SET_DEFAULT);
    if (ret == ESP_OK) {
        device->io.delay_ms(device->io.ctx, 2U);
        ret = reg_write(device, REG_IO_CONF2, 0x3CU);
    }
    if (ret == ESP_OK) {
        ret = reg_read(device, REG_IC_IDENTITY, &device->identity);
    }
    const uint8_t chip_type = device->identity & IC_TYPE_MASK;
    if (ret == ESP_OK && chip_type != IC_TYPE_ST25R3916 &&
        (chip_type != IC_TYPE_ST25R3916B ||
         (device->identity & 0x07U) == 0U)) {
        ret = ESP_ERR_NOT_FOUND;
    }
    if (ret == ESP_OK) {
        ret = reg_write(device, REG_OP_CONTROL, OP_CONTROL_EN);
        device->io.delay_ms(device->io.ctx, 10U);
    }
    uint8_t aux_display = 0U;
    if (ret == ESP_OK) {
        ret = reg_read(device, REG_AUX_DISPLAY, &aux_display);
    }
    if (ret == ESP_OK && (aux_display & AUX_DISPLAY_OSC_OK) == 0U) {
        ret = ESP_ERR_INVALID_STATE;
    }
    const register_config_t common_config[] = {
        {REG_IO_CONF1, 0x07U}, {REG_B_EMD_SUP_CONF, 0x40U},
        {REG_B_PASSIVE_TARGET, 0x50U}, {REG_MODE, 0x08U},
        {REG_BIT_RATE, 0x00U}, {0x0BU, 0x08U},
        {0x0DU, 0x00U}, {0x0EU, 0x00U},
        {REG_B_CORR_CONF1, 0x51U}, {REG_B_CORR_CONF2, 0x00U},
        {REG_IRQ_MASK_MAIN, 0xE3U}, {REG_IRQ_MASK_TIMER_NFC, 0xBFU},
        {REG_IRQ_MASK_ERROR_WUP, 0x0FU}, {REG_IRQ_MASK_TARGET, 0xFFU},
    };
    for (size_t i = 0U; ret == ESP_OK &&
         i < sizeof(common_config) / sizeof(common_config[0]); i++) {
        ret = reg_write(device, common_config[i].reg, common_config[i].value);
    }
    const register_config_t base_config[] = {
        {REG_ANT_TUNE_A, 0x82U}, {REG_ANT_TUNE_B, 0x82U},
        {REG_TX_DRIVER, 0x70U}, {REG_B_AUX_MOD, 0x10U},
        {REG_B_PT_MOD, 0x5FU}, {REG_B_RES_AM_MOD, 0x80U},
        {REG_B_OVERSHOOT_CONF1, 0x40U}, {REG_B_OVERSHOOT_CONF2, 0x03U},
        {REG_B_UNDERSHOOT_CONF1, 0x40U}, {REG_B_UNDERSHOOT_CONF2, 0x03U},
        {0x0CU, 0x2DU},
    };
    const register_config_t b_config[] = {
        {REG_ANT_TUNE_A, 0xC5U}, {REG_ANT_TUNE_B, 0xE3U},
        {REG_TX_DRIVER, 0xF0U}, {REG_B_AUX_MOD, 0x94U},
        {REG_B_PT_MOD, 0x2EU}, {REG_B_AWS_CONF1, 0x09U},
        {REG_B_AWS_CONF2, 0x18U}, {REG_B_AWS_TIME1, 0x01U},
        {REG_B_AWS_TIME3, 0x79U}, {REG_B_AWS_TIME4, 0x07U},
        {0x0CU, 0xEDU},
    };
    const register_config_t *chip_config = chip_type == IC_TYPE_ST25R3916B
        ? b_config : base_config;
    const size_t selected_count = chip_type == IC_TYPE_ST25R3916B
        ? sizeof(b_config) / sizeof(b_config[0])
        : sizeof(base_config) / sizeof(base_config[0]);
    for (size_t i = 0U; ret == ESP_OK && i < selected_count; i++) {
        ret = reg_write(device, chip_config[i].reg, chip_config[i].value);
    }
    if (ret == ESP_OK) {
        ret = command(device, CMD_ADJUST_REGULATORS);
        device->io.delay_ms(device->io.ctx, 2U);
    }
    if (ret == ESP_OK) {
        ret = clear_irqs(device);
    }
    device->initialized = ret == ESP_OK;
    return ret;
}

esp_err_t st25r3916_deinit(st25r3916_t *device)
{
    if (device == NULL || !device->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t ret = reg_write(device, REG_OP_CONTROL, OP_CONTROL_EN);
    device->initialized = false;
    return ret;
}

esp_err_t st25r3916_nfca_scan(st25r3916_t *device,
                              uint32_t timeout_ms,
                              st25r3916_nfca_tag_t *tag)
{
    if (device == NULL || !device->initialized || timeout_ms == 0U || tag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(tag, 0, sizeof(*tag));
    esp_err_t ret = reg_write(device, REG_OP_CONTROL,
                              OP_CONTROL_EN | OP_CONTROL_RX_EN | OP_CONTROL_TX_EN);
    if (ret != ESP_OK) {
        return ret;
    }
    device->io.delay_ms(device->io.ctx, 5U);
    const int64_t deadline = device->io.time_ms(device->io.ctx) + timeout_ms;
    do {
        ret = reqa(device, tag->atqa, 12U);
        if (ret == ESP_OK) {
            break;
        }
        device->io.delay_ms(device->io.ctx, 10U);
    } while (device->io.time_ms(device->io.ctx) < deadline);

    static const uint8_t select_commands[] = {0x93U, 0x95U, 0x97U};
    bool uid_complete = false;
    for (size_t cascade = 0U; ret == ESP_OK && cascade < 3U; cascade++) {
        const uint8_t anticollision[] = {select_commands[cascade], 0x20U};
        uint8_t response[5] = {0};
        size_t response_len = 0U;
        ret = reg_change(device, REG_ISO14443A_NFC,
                         ISO14443A_ANTICOLLISION, ISO14443A_ANTICOLLISION);
        if (ret == ESP_OK) {
            ret = transceive(device, anticollision, sizeof(anticollision),
                             false, false, response, sizeof(response),
                             &response_len, 20U);
        }
        if (ret == ESP_OK && (response_len != 5U ||
            (uint8_t)(response[0] ^ response[1] ^ response[2] ^ response[3]) != response[4])) {
            ret = ESP_ERR_INVALID_RESPONSE;
        }
        const uint8_t select[] = {
            select_commands[cascade], 0x70U,
            response[0], response[1], response[2], response[3], response[4],
        };
        uint8_t sak[1] = {0};
        if (ret == ESP_OK) {
            ret = reg_change(device, REG_ISO14443A_NFC,
                             ISO14443A_ANTICOLLISION, 0U);
        }
        if (ret == ESP_OK) {
            ret = transceive(device, select, sizeof(select), true, true,
                             sak, sizeof(sak), &response_len, 20U);
        }
        if (ret == ESP_OK && response_len != 1U) {
            ret = ESP_ERR_INVALID_RESPONSE;
        }
        if (ret != ESP_OK) {
            break;
        }
        tag->sak = sak[0];
        if (response[0] == 0x88U) {
            if (tag->uid_len > ST25R3916_UID_MAX - 3U || (sak[0] & 0x04U) == 0U) {
                ret = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            memcpy(&tag->uid[tag->uid_len], &response[1], 3U);
            tag->uid_len += 3U;
        } else {
            if (tag->uid_len > ST25R3916_UID_MAX - 4U || (sak[0] & 0x04U) != 0U) {
                ret = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            memcpy(&tag->uid[tag->uid_len], response, 4U);
            tag->uid_len += 4U;
            uid_complete = true;
            break;
        }
    }
    if (ret == ESP_OK && !uid_complete) {
        ret = ESP_ERR_INVALID_RESPONSE;
    }
    const esp_err_t field_ret = reg_write(device, REG_OP_CONTROL, OP_CONTROL_EN);
    return ret == ESP_OK ? field_ret : ret;
}
