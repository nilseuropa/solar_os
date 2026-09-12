#include "solar_os_st25r3916.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

/*
 * ST25R3916 NFC reader/writer for the LilyGO T-LoRa-Pager.
 *
 * SPI protocol (CPOL=0, CPHA=0, CS active low):
 *   Write register : [0x00 | (reg & 0x3F), value]
 *   Read  register : [0x40 | (reg & 0x3F), 0x00]  → byte[1]
 *   Direct command : [0xC0 | (cmd & 0x3F)]
 *   FIFO write     : [0x80, data...]
 *   FIFO read      : [0x9F, 0x00...]  → bytes[1..]
 *
 * Power via XL9555 Port0 bit5 (NFC_EN), asserted HIGH by default.
 * Chip identity register 0x1C returns 0xA0 (ST25R3916) or 0xA1 (B variant).
 */

#define ST25R3916_SPI_SPEED_HZ 4000000U
#define ST25R3916_SPI_MODE     0  /* CPOL=0, CPHA=0 */

/* Register map (selected subset for ISO 14443A) */
#define REG_IO_CONF1           0x00U
#define REG_IO_CONF2           0x01U
#define REG_OP_CONTROL         0x02U
#define REG_MODE               0x03U
#define REG_BIT_RATE           0x04U
#define REG_ISO14443A_NFC      0x05U
#define REG_AUX                0x09U
#define REG_TX_DRIVER          0x10U
#define REG_FIELD_THRESHOLD_ACT 0x11U
#define REG_FIELD_THRESHOLD_DEACT 0x12U
#define REG_TIMER_EMV_CONTROL  0x15U
#define REG_NO_RESPONSE_TIMER1 0x16U
#define REG_NO_RESPONSE_TIMER2 0x17U
#define REG_GPT_CONTROL        0x18U
#define REG_MASK_RX_TIMER      0x1AU
#define REG_PASSIVE_TARGET_DEF 0x1BU
#define REG_IC_IDENTITY        0x1CU  /* returns 0xA0 (ST25R3916) / 0xA1 (B) */
#define REG_EMD_SUP_CONF       0x0EU
#define REG_SUBC_START_TIME    0x0FU
#define REG_RX_CONF1           0x06U
#define REG_RX_CONF2           0x07U
#define REG_RX_CONF3           0x08U
#define REG_FIFO_STATUS1       0x1DU
#define REG_FIFO_STATUS2       0x1EU
#define REG_COLLISION_STATUS   0x1FU
#define REG_NUM_TX_BYTES1      0x20U
#define REG_NUM_TX_BYTES2      0x21U
#define REG_NFCIP1_BIT_RATE    0x22U
#define REG_AUX_DISPLAY        0x23U
#define REG_RSSI_RESULT        0x24U
#define REG_GAIN_RED_STATE     0x25U
#define REG_CAP_SENSOR_CONTROL 0x26U
#define REG_CAP_SENSOR_RESULT  0x27U
#define REG_AUXILIARY_DISPLAY  0x28U
#define REG_OVERSHOOT_CONF1    0x30U
#define REG_OVERSHOOT_CONF2    0x31U
#define REG_UNDERSHOOT_CONF1   0x32U
#define REG_UNDERSHOOT_CONF2   0x33U
#define REG_INTERRUPT_MASK_MAIN 0x34U
#define REG_INTERRUPT_MASK_AUX 0x35U
#define REG_INTERRUPT_MAIN     0x36U
#define REG_INTERRUPT_AUX      0x37U

/* Direct commands */
#define CMD_SET_DEFAULT        0x01U
#define CMD_STOP               0x02U
#define CMD_TRANSMIT_NO_CRC    0x04U
#define CMD_TRANSMIT_WITH_CRC  0x05U
#define CMD_RECEIVE            0x06U
#define CMD_UNMASK_RX          0x07U
#define CMD_MEASURE_RSSI       0x0DU
#define CMD_ADJUST_REGULATORS  0x0EU
#define CMD_CAL_MODULATION     0x0FU
#define CMD_CAL_ANTENNA        0x10U
#define CMD_CAL_DRIVER         0x14U
#define CMD_CLEAR_FIFO         0x0AU
#define CMD_TRANSMIT_REQA      0x26U  /* REQA (7-bit short frame) */
#define CMD_TRANSMIT_WUPA      0x28U  /* WUPA */
#define CMD_NFC_INITIAL_FIELD_ON  0x17U
#define CMD_NFC_RESPONSE_FIELD_ON 0x18U

/* OP_CONTROL bits */
#define OP_CONTROL_RX_EN    0x80U
#define OP_CONTROL_RX_MAN   0x40U
#define OP_CONTROL_TX_EN    0x08U
#define OP_CONTROL_RF_EN    0x02U

/* MODE register values */
#define MODE_ISO14443A      0x08U  /* nfc_ar=0, om[3:0]=0x8 = ISO14443A/NFC-A */

/* SPI command byte prefixes */
#define SPI_CMD_WRITE  0x00U
#define SPI_CMD_READ   0x40U
#define SPI_CMD_DIRECT 0xC0U
#define SPI_CMD_FIFO_W 0x80U
#define SPI_CMD_FIFO_R 0x9FU

typedef struct {
    bool active;
    bool chip_ready;  /* true after successful chip init */
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs_pin;
    int irq_pin;
} solar_os_st25r3916_device_t;

static const char *TAG = "st25r3916";
static solar_os_st25r3916_device_t nfc_device;

/* ---- Low-level SPI helpers ---- */

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = {(uint8_t)(SPI_CMD_WRITE | (reg & 0x3FU)), val};
    uint8_t rx[2] = {0};
    return solar_os_bus_spi_transfer(nfc_device.spi_bus, nfc_device.cs_pin,
                                     ST25R3916_SPI_MODE, ST25R3916_SPI_SPEED_HZ,
                                     tx, rx, 2);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    uint8_t tx[2] = {(uint8_t)(SPI_CMD_READ | (reg & 0x3FU)), 0x00U};
    uint8_t rx[2] = {0};
    esp_err_t err = solar_os_bus_spi_transfer(nfc_device.spi_bus, nfc_device.cs_pin,
                                              ST25R3916_SPI_MODE, ST25R3916_SPI_SPEED_HZ,
                                              tx, rx, 2);
    if (err == ESP_OK) {
        *val = rx[1];
    }
    return err;
}

static esp_err_t direct_cmd(uint8_t cmd)
{
    uint8_t tx[1] = {(uint8_t)(SPI_CMD_DIRECT | (cmd & 0x3FU))};
    uint8_t rx[1] = {0};
    return solar_os_bus_spi_transfer(nfc_device.spi_bus, nfc_device.cs_pin,
                                     ST25R3916_SPI_MODE, ST25R3916_SPI_SPEED_HZ,
                                     tx, rx, 1);
}

static esp_err_t fifo_read(uint8_t *buf, size_t len)
{
    if (len == 0 || buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tx[33] = {SPI_CMD_FIFO_R};  /* max 32 bytes useful FIFO + 1 cmd byte */
    uint8_t rx[33] = {0};
    const size_t total = 1 + len;
    if (total > sizeof(tx)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_bus_spi_transfer(nfc_device.spi_bus, nfc_device.cs_pin,
                                              ST25R3916_SPI_MODE, ST25R3916_SPI_SPEED_HZ,
                                              tx, rx, total);
    if (err == ESP_OK) {
        memcpy(buf, &rx[1], len);
    }
    return err;
}

static esp_err_t fifo_write(const uint8_t *buf, size_t len)
{
    if (len == 0 || buf == NULL || len > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tx[33] = {SPI_CMD_FIFO_W};
    uint8_t rx[33] = {0};
    memcpy(&tx[1], buf, len);
    return solar_os_bus_spi_transfer(nfc_device.spi_bus, nfc_device.cs_pin,
                                     ST25R3916_SPI_MODE, ST25R3916_SPI_SPEED_HZ,
                                     tx, rx, 1 + len);
}

static esp_err_t reg_set_bits(uint8_t reg, uint8_t mask)
{
    uint8_t val = 0;
    ESP_RETURN_ON_ERROR(reg_read(reg, &val), TAG, "reg_read failed");
    return reg_write(reg, val | mask);
}

static esp_err_t reg_clear_bits(uint8_t reg, uint8_t mask)
{
    uint8_t val = 0;
    ESP_RETURN_ON_ERROR(reg_read(reg, &val), TAG, "reg_read failed");
    return reg_write(reg, val & (uint8_t)~mask);
}

/* ---- Chip init ---- */

static esp_err_t st25r3916_chip_init(void)
{
    uint8_t id = 0;

    ESP_RETURN_ON_ERROR(direct_cmd(CMD_SET_DEFAULT), TAG, "SET_DEFAULT failed");
    vTaskDelay(pdMS_TO_TICKS(2));

    ESP_RETURN_ON_ERROR(reg_read(REG_IC_IDENTITY, &id), TAG, "IC identity read failed");
    if ((id & 0xF8U) != 0xA0U) {
        ESP_LOGE(TAG, "unexpected IC identity 0x%02X (expected 0xA0 or 0xA1)", id);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "ST25R3916%s detected (IC identity 0x%02X)",
             (id & 0x01U) ? "B" : "", id);

    ESP_RETURN_ON_ERROR(direct_cmd(CMD_ADJUST_REGULATORS), TAG, "adjust regulators failed");
    vTaskDelay(pdMS_TO_TICKS(2));

    /* Mask all interrupts; we poll FIFO/status registers directly. */
    ESP_RETURN_ON_ERROR(reg_write(REG_INTERRUPT_MASK_MAIN, 0xFFU), TAG, "mask irq main");
    ESP_RETURN_ON_ERROR(reg_write(REG_INTERRUPT_MASK_AUX,  0xFFU), TAG, "mask irq aux");

    /* TX/RX clocks on, RF field off until scan. */
    ESP_RETURN_ON_ERROR(reg_write(REG_OP_CONTROL, OP_CONTROL_TX_EN | OP_CONTROL_RX_EN),
                        TAG, "op_control init");

    return ESP_OK;
}

/* ---- ISO 14443A helpers ---- */

static esp_err_t iso14443a_field_on(void)
{
    ESP_RETURN_ON_ERROR(reg_set_bits(REG_OP_CONTROL, OP_CONTROL_RF_EN), TAG, "rf on");
    vTaskDelay(pdMS_TO_TICKS(5));  /* RF rise time */
    return ESP_OK;
}

static esp_err_t iso14443a_field_off(void)
{
    return reg_clear_bits(REG_OP_CONTROL, OP_CONTROL_RF_EN);
}

static esp_err_t iso14443a_set_mode(void)
{
    /* ISO 14443A / NFC-A, 106 kbps */
    ESP_RETURN_ON_ERROR(reg_write(REG_MODE, MODE_ISO14443A), TAG, "set mode");
    ESP_RETURN_ON_ERROR(reg_write(REG_BIT_RATE, 0x00U), TAG, "set bit rate 106k");
    return ESP_OK;
}

/* Wait for FIFO to contain at least `min_bytes` bytes, with timeout. */
static esp_err_t wait_fifo(uint8_t min_bytes, uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    while (esp_timer_get_time() < deadline) {
        uint8_t fs1 = 0;
        esp_err_t err = reg_read(REG_FIFO_STATUS1, &fs1);
        if (err != ESP_OK) {
            return err;
        }
        /* FIFO_STATUS1 bits [6:0] = number of bytes in receive FIFO */
        if ((fs1 & 0x7FU) >= min_bytes) {
            return ESP_OK;
        }
        vTaskDelay(1);
    }
    return ESP_ERR_TIMEOUT;
}

/* Send REQA and receive ATQA (2 bytes). */
static esp_err_t iso14443a_reqa(uint8_t *atqa)
{
    ESP_RETURN_ON_ERROR(direct_cmd(CMD_CLEAR_FIFO), TAG, "clear fifo");

    /* Mask collision bits off — allow incomplete byte framing for short frame */
    ESP_RETURN_ON_ERROR(reg_set_bits(REG_ISO14443A_NFC, 0x01U), TAG, "iso14443a short frame");

    /* Transmit REQA (0x26) as a 7-bit short frame */
    ESP_RETURN_ON_ERROR(direct_cmd(CMD_TRANSMIT_REQA), TAG, "REQA tx");

    /* ATQA is 2 bytes */
    esp_err_t err = wait_fifo(2, 10);
    if (err != ESP_OK) {
        return ESP_ERR_NOT_FOUND;  /* No tag present */
    }

    ESP_RETURN_ON_ERROR(fifo_read(atqa, 2), TAG, "ATQA fifo read");
    return ESP_OK;
}

/* ISO 14443A anticollision + SELECT for single / double / triple UID.
 * Fills uid/uid_len/sak in *tag on success. */
static esp_err_t iso14443a_anticoll_select(solar_os_st25r3916_tag_t *tag)
{
    static const uint8_t sel_cmds[3]   = {0x93U, 0x95U, 0x97U};
    uint8_t uid_buf[10] = {0};
    uint8_t uid_total = 0;

    for (int cascade = 0; cascade < 3; cascade++) {
        /* Anticollision: SEL + NVB=0x20, no UID bits known */
        uint8_t acoll[2] = {sel_cmds[cascade], 0x20U};
        ESP_RETURN_ON_ERROR(direct_cmd(CMD_CLEAR_FIFO), TAG, "clear fifo");
        ESP_RETURN_ON_ERROR(fifo_write(acoll, 2), TAG, "acoll fifo write");
        ESP_RETURN_ON_ERROR(direct_cmd(CMD_TRANSMIT_WITH_CRC), TAG, "acoll tx");

        esp_err_t err = wait_fifo(5, 20);
        if (err != ESP_OK) {
            return ESP_ERR_NOT_FOUND;
        }

        uint8_t uid_cl[5] = {0};  /* CT + 3 UID bytes + BCC (4) or 4 bytes + BCC (5) */
        ESP_RETURN_ON_ERROR(fifo_read(uid_cl, 5), TAG, "acoll fifo read");

        /* SELECT: SEL + NVB=0x70 + 5 data bytes (uid_cl) */
        uint8_t sel[7] = {sel_cmds[cascade], 0x70U,
                          uid_cl[0], uid_cl[1], uid_cl[2], uid_cl[3], uid_cl[4]};
        ESP_RETURN_ON_ERROR(direct_cmd(CMD_CLEAR_FIFO), TAG, "clear fifo");
        ESP_RETURN_ON_ERROR(fifo_write(sel, 7), TAG, "sel fifo write");
        ESP_RETURN_ON_ERROR(direct_cmd(CMD_TRANSMIT_WITH_CRC), TAG, "sel tx");

        err = wait_fifo(1, 20);
        if (err != ESP_OK) {
            return ESP_ERR_NOT_FOUND;
        }

        uint8_t sak = 0;
        ESP_RETURN_ON_ERROR(fifo_read(&sak, 1), TAG, "SAK fifo read");

        if (uid_cl[0] == 0x88U) {
            /* CT byte — cascade tag, take bytes 1..3 */
            memcpy(&uid_buf[uid_total], &uid_cl[1], 3);
            uid_total += 3;
            /* SAK bit 2 set = more cascades */
            if ((sak & 0x04U) == 0U) {
                break;
            }
        } else {
            /* Last cascade level — take bytes 0..3 */
            memcpy(&uid_buf[uid_total], &uid_cl[0], 4);
            uid_total += 4;
            tag->sak = sak;
            break;
        }
        tag->sak = sak;
    }

    if (uid_total == 0 || uid_total > SOLAR_OS_ST25R3916_UID_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    memcpy(tag->uid, uid_buf, uid_total);
    tag->uid_len = uid_total;
    return ESP_OK;
}

/* ---- Binding parser ---- */

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *spi_bus,
                                size_t spi_bus_len,
                                int *cs_pin,
                                int *irq_pin)
{
    bool have_spi = false;
    bool have_cs  = false;

    if (bindings == NULL || spi_bus == NULL || cs_pin == NULL || irq_pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    spi_bus[0] = '\0';
    *cs_pin  = -1;
    *irq_pin = -1;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        switch (b->kind) {
        case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
            if (have_spi) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(spi_bus, b->target, spi_bus_len);
            have_spi = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
            if (have_cs) {
                return ESP_ERR_INVALID_ARG;
            }
            *cs_pin = b->value;
            have_cs = true;
            /* SPI_CS binding may also carry the bus name */
            if (!have_spi && b->target[0] != '\0') {
                strlcpy(spi_bus, b->target, spi_bus_len);
                have_spi = true;
            }
            break;
        case SOLAR_OS_EXPANSION_BINDING_GPIO:
            if (b->role != NULL && strcmp(b->role, "irq") == 0) {
                *irq_pin = b->value;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    return (have_spi && have_cs &&
            solar_os_expansion_find_spi_bus(spi_bus, NULL, NULL) &&
            solar_os_expansion_spi_cs_allowed(spi_bus, *cs_pin))
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

/* ---- Public API ---- */

esp_err_t solar_os_st25r3916_attach(const char *name,
                                    const solar_os_expansion_binding_t *bindings,
                                    size_t binding_count)
{
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    int cs_pin  = -1;
    int irq_pin = -1;

    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (nfc_device.active) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count,
                                       spi_bus, sizeof(spi_bus),
                                       &cs_pin, &irq_pin),
                        TAG, "invalid bindings");

    memset(&nfc_device, 0, sizeof(nfc_device));
    strlcpy(nfc_device.name, name, sizeof(nfc_device.name));
    strlcpy(nfc_device.spi_bus, spi_bus, sizeof(nfc_device.spi_bus));
    nfc_device.cs_pin  = cs_pin;
    nfc_device.irq_pin = irq_pin;
    nfc_device.active  = true;

    /* Attempt chip init now; if NFC_EN is low the chip won't respond and
     * chip_ready stays false. scan() will retry init on first use. */
    esp_err_t err = st25r3916_chip_init();
    nfc_device.chip_ready = (err == ESP_OK);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: chip not responding at attach (NFC power off?); "
                 "will retry on first scan", name);
    }

    ESP_LOGI(TAG, "%s attached on %s cs=%d%s",
             name, spi_bus, cs_pin,
             irq_pin >= 0 ? " (irq wired)" : "");
    return ESP_OK;
}

bool solar_os_st25r3916_is_ready(void)
{
    return nfc_device.active && nfc_device.chip_ready;
}

void solar_os_st25r3916_reset_chip(void)
{
    nfc_device.chip_ready = false;
}

esp_err_t solar_os_st25r3916_detach(const char *name)
{
    if (!nfc_device.active || name == NULL || strcmp(nfc_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    iso14443a_field_off();
    direct_cmd(CMD_STOP);
    memset(&nfc_device, 0, sizeof(nfc_device));
    return ESP_OK;
}

esp_err_t solar_os_st25r3916_scan(uint32_t timeout_ms, solar_os_st25r3916_tag_t *tag)
{
    if (!nfc_device.active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nfc_device.chip_ready) {
        esp_err_t err = st25r3916_chip_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "chip init failed (is NFC power on?): %s", esp_err_to_name(err));
            return err;
        }
        nfc_device.chip_ready = true;
    }

    memset(tag, 0, sizeof(*tag));

    ESP_RETURN_ON_ERROR(iso14443a_set_mode(), TAG, "set mode");
    ESP_RETURN_ON_ERROR(iso14443a_field_on(), TAG, "field on");

    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    esp_err_t result = ESP_ERR_NOT_FOUND;

    do {
        uint8_t atqa[2] = {0};
        esp_err_t err = iso14443a_reqa(atqa);
        if (err == ESP_OK) {
            tag->atqa[0] = atqa[0];
            tag->atqa[1] = atqa[1];
            err = iso14443a_anticoll_select(tag);
            if (err == ESP_OK) {
                result = ESP_OK;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (esp_timer_get_time() < deadline);

    iso14443a_field_off();
    return result;
}
