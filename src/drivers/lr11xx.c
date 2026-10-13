#include "lr11xx.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

/*
 * Semtech LR11xx command-set driver for the LR1110, LR1120 and LR1121: one
 * part with three front ends, told apart by what the silicon reports about
 * itself (see lr11xx_probe()).
 *
 * The transport is close to the SX126x one (see sx1262.c) but differs in two
 * ways that keep this out of that driver:
 *
 *   - An opcode is two bytes, a group byte and a command byte.
 *   - A command that returns data needs two chip-select windows. The first
 *     carries the opcode and parameters; BUSY rises while the part prepares
 *     the answer; the second clocks out a status byte and the response with
 *     no opcode.
 *
 * A bare read with no opcode returns stat1, stat2 and the interrupt word, so
 * there is no GetIrqStatus command. Frequency is plain hertz and the LoRa
 * sync word is one command. Opcodes, IRQ bits, enum codes and parameter
 * layouts are from Semtech's published LR11xx driver (SWDR001).
 */

/* System group. */
#define LR11XX_OP_GET_VERSION 0x0101U
#define LR11XX_OP_GET_ERRORS 0x010DU
#define LR11XX_OP_CLEAR_ERRORS 0x010EU
#define LR11XX_OP_CALIBRATE 0x010FU
#define LR11XX_OP_SET_REG_MODE 0x0110U
#define LR11XX_OP_CALIBRATE_IMAGE 0x0111U
#define LR11XX_OP_SET_DIO_AS_RF_SWITCH 0x0112U
#define LR11XX_OP_SET_DIO_IRQ_PARAMS 0x0113U
#define LR11XX_OP_CLEAR_IRQ 0x0114U
#define LR11XX_OP_SET_TCXO_MODE 0x0117U
#define LR11XX_OP_SET_SLEEP 0x011BU
#define LR11XX_OP_SET_STANDBY 0x011CU

/* Register/memory group. */
#define LR11XX_OP_WRITE_BUFFER8 0x0109U
#define LR11XX_OP_READ_BUFFER8 0x010AU
#define LR11XX_OP_WRITE_REG_MEM32_MASK 0x010CU

/*
 * Waking from a retention sleep leaves LoRa transmissions with high
 * adjacent-channel power on some LR1110 and LR1120 firmware. Clearing bit 30
 * of this register after the wake is Semtech's published workaround, and is
 * harmless on unaffected parts.
 */
#define LR11XX_REG_HIGH_ACP 0x00F30054UL
#define LR11XX_REG_HIGH_ACP_BIT (1UL << 30)

/* SetSleep config byte: bit 0 retains configuration across the sleep, bit 1
 * arms the RTC wake-up. */
#define LR11XX_SLEEP_WARM_START 0x01U

/* Radio group. */
#define LR11XX_OP_GET_RX_BUFFER_STATUS 0x0203U
#define LR11XX_OP_GET_PACKET_STATUS 0x0204U
#define LR11XX_OP_GET_RSSI_INST 0x0205U
#define LR11XX_OP_SET_RX 0x0209U
#define LR11XX_OP_SET_TX 0x020AU
#define LR11XX_OP_SET_RF_FREQUENCY 0x020BU
#define LR11XX_OP_SET_PACKET_TYPE 0x020EU
#define LR11XX_OP_SET_MODULATION_PARAMS 0x020FU
#define LR11XX_OP_SET_PACKET_PARAMS 0x0210U
#define LR11XX_OP_SET_TX_PARAMS 0x0211U
#define LR11XX_OP_SET_PA_CONFIG 0x0215U
#define LR11XX_OP_SET_LORA_SYNC_WORD 0x022BU

#define LR11XX_STANDBY_RC 0x00U
#define LR11XX_REG_MODE_DCDC 0x01U

#define LR11XX_PACKET_TYPE_LORA 0x02U

/* IRQ mask is 32 bits wide here, not the SX126x's 16. */
#define LR11XX_IRQ_TX_DONE 0x00000004UL
#define LR11XX_IRQ_RX_DONE 0x00000008UL
#define LR11XX_IRQ_HEADER_ERROR 0x00000040UL
#define LR11XX_IRQ_CRC_ERROR 0x00000080UL
#define LR11XX_IRQ_TIMEOUT 0x00000400UL
#define LR11XX_IRQ_ALL 0xFFFFFFFFUL

/* Calibrate takes a bit per block. 0x3F is every block the sub-GHz paths use. */
#define LR11XX_CALIBRATE_ALL 0x3FU

/* PaSel picks which of the three power amplifiers the part routes TX through. */
#define LR11XX_PA_SEL_LP 0x00U
#define LR11XX_PA_SEL_HP 0x01U
#define LR11XX_PA_REG_SUPPLY_VREG 0x00U
#define LR11XX_PA_REG_SUPPLY_VBAT 0x01U

/* Bits 3:1 of the first status byte say how the last command went, and the
 * same bits of the second which mode the part is in. Neither field uses its
 * whole range, which is what tells a part answering from a bus nobody is
 * driving: that reads all ones. A bus pulled low reads all zeros, which is
 * a command that failed and a part asleep; a part asleep holds BUSY and
 * never answers a read at all, so no read can come back saying so. */
#define LR11XX_STAT1_COMMAND(stat1) (((stat1) >> 1) & 0x07U)
#define LR11XX_COMMAND_PERR 0x01U
#define LR11XX_COMMAND_LAST 0x03U
#define LR11XX_STAT2_MODE(stat2) (((stat2) >> 1) & 0x07U)
#define LR11XX_MODE_SLEEP 0x00U
#define LR11XX_MODE_LAST 0x06U

#define LR11XX_BUSY_TIMEOUT_US 1000000ULL
/* BUSY drops within microseconds of most commands, so the wait starts as a
 * spin; a part still busy after this long is calibrating or not answering,
 * and the wait then sleeps a tick at a time so everything else can run. */
#define LR11XX_BUSY_SPIN_US 2000ULL
#define LR11XX_POLL_INTERVAL_MS 4U
/* SetTx/SetRx timeouts and the TCXO start-up delay count in 1/32768 s ticks. */
#define LR11XX_TICK_NS 30518ULL

/* One opcode plus the longest parameter block the driver sends, which is a
 * WriteBuffer8 of a full packet. */
#define LR11XX_MAX_FRAME (2U + 1U + LR11XX_MAX_PACKET_LEN)

const char *lr11xx_part_name(lr11xx_part_t part)
{
    switch (part) {
    case LR11XX_PART_LR1110: return "LR1110";
    case LR11XX_PART_LR1120: return "LR1120";
    case LR11XX_PART_LR1121: return "LR1121";
    case LR11XX_PART_UNKNOWN:
    default: return "LR11xx";
    }
}

static void lr11xx_put_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static esp_err_t lr11xx_lock(lr11xx_t *dev)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Made by lr11xx_init(), so there is none before it. */
    if (dev->mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(dev->mutex, portMAX_DELAY);
    return ESP_OK;
}

static void lr11xx_unlock(lr11xx_t *dev)
{
    if (dev != NULL && dev->mutex != NULL) {
        xSemaphoreGive(dev->mutex);
    }
}

static esp_err_t lr11xx_wait_busy(lr11xx_t *dev)
{
    const int64_t start = esp_timer_get_time();
    while (gpio_get_level(dev->busy_pin) != 0) {
        const uint64_t waited = (uint64_t)(esp_timer_get_time() - start);
        if (waited > LR11XX_BUSY_TIMEOUT_US) {
            /* Not a timeout to the caller, which has its own: a part that
             * holds BUSY this long is not answering. */
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (waited < LR11XX_BUSY_SPIN_US) {
            esp_rom_delay_us(5);
        } else {
            vTaskDelay(1);
        }
    }
    return ESP_OK;
}

/* First chip-select window: opcode and parameters out, nothing read back. */
static esp_err_t lr11xx_write(lr11xx_t *dev,
                              uint16_t opcode,
                              const uint8_t *params,
                              size_t param_len)
{
    uint8_t tx[LR11XX_MAX_FRAME];
    const size_t total = 2U + param_len;

    if (total > sizeof(tx)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lr11xx_wait_busy(dev);
    if (err != ESP_OK) {
        return err;
    }
    tx[0] = (uint8_t)(opcode >> 8);
    tx[1] = (uint8_t)(opcode & 0xFFU);
    if (param_len > 0) {
        memcpy(&tx[2], params, param_len);
    }
    return solar_os_bus_spi_transfer(dev->spi_bus, dev->cs_pin, 0, dev->speed_hz,
                                     tx, NULL, total);
}

/* Second chip-select window: a status byte followed by the response. No
 * opcode is re-sent - the part already knows what was asked. */
static esp_err_t lr11xx_read_response(lr11xx_t *dev, uint8_t *resp, size_t resp_len)
{
    uint8_t tx[1U + LR11XX_MAX_PACKET_LEN];
    uint8_t rx[1U + LR11XX_MAX_PACKET_LEN];
    const size_t total = 1U + resp_len;

    if (total > sizeof(tx)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lr11xx_wait_busy(dev);
    if (err != ESP_OK) {
        return err;
    }
    memset(tx, 0, total);
    err = solar_os_bus_spi_transfer(dev->spi_bus, dev->cs_pin, 0, dev->speed_hz,
                                    tx, rx, total);
    if (err != ESP_OK) {
        return err;
    }
    /* A command the part failed or could not parse has no answer to give,
     * and neither has a bus with no part on it. */
    if (LR11XX_STAT1_COMMAND(rx[0]) <= LR11XX_COMMAND_PERR ||
        LR11XX_STAT1_COMMAND(rx[0]) > LR11XX_COMMAND_LAST) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (resp_len > 0 && resp != NULL) {
        memcpy(resp, &rx[1], resp_len);
    }
    return ESP_OK;
}

/* A bare read with no command in front of it. The part answers any such read
 * with stat1, stat2 and the 32-bit interrupt word, which is how status is
 * fetched: there is no GetIrqStatus opcode on this family. */
static esp_err_t lr11xx_direct_read(lr11xx_t *dev, uint8_t *out, size_t len)
{
    uint8_t tx[8] = {0};
    if (len == 0 || len > sizeof(tx)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = lr11xx_wait_busy(dev);
    if (err != ESP_OK) {
        return err;
    }
    return solar_os_bus_spi_transfer(dev->spi_bus, dev->cs_pin, 0, dev->speed_hz,
                                     tx, out, len);
}

/*
 * A command that returns nothing, and how the part took it. The part says so
 * only in the status of the next read, so without that read a command it
 * refused looks the same as one it ran. Not for a command with an answer,
 * whose answer this read would take, nor for SetSleep, which it would wake.
 */
static esp_err_t lr11xx_command(lr11xx_t *dev,
                                uint16_t opcode,
                                const uint8_t *params,
                                size_t param_len)
{
    esp_err_t err = lr11xx_write(dev, opcode, params, param_len);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t status[2] = {0};
    err = lr11xx_direct_read(dev, status, sizeof(status));
    if (err != ESP_OK) {
        return err;
    }
    if (LR11XX_STAT1_COMMAND(status[0]) <= LR11XX_COMMAND_PERR ||
        LR11XX_STAT1_COMMAND(status[0]) > LR11XX_COMMAND_LAST ||
        LR11XX_STAT2_MODE(status[1]) > LR11XX_MODE_LAST) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* Command plus its response read. Both windows must happen under the device
 * mutex: the part holds exactly one pending answer, so a second caller
 * slipping a command in between would collect the wrong bytes. Callers here
 * always hold the mutex already. */
static esp_err_t lr11xx_query(lr11xx_t *dev,
                              uint16_t opcode,
                              const uint8_t *params,
                              size_t param_len,
                              uint8_t *resp,
                              size_t resp_len)
{
    const esp_err_t err = lr11xx_write(dev, opcode, params, param_len);
    if (err != ESP_OK) {
        return err;
    }
    return lr11xx_read_response(dev, resp, resp_len);
}

/*
 * A sleeping LR11xx wakes on a falling edge of chip select held low for at
 * least 100 us. The SPI peripheral owns chip select, so it is held low by
 * clocking, and the bytes must be zero: the part reads MOSI here and zero is
 * the NOP opcode. The high-ACP workaround follows, since this wake is what
 * triggers it.
 */
static esp_err_t lr11xx_poke(lr11xx_t *dev)
{
    uint8_t buffer[64] = {0};
    /* Bytes needed to span 100 us at the bus clock, plus one for rounding. */
    size_t len = (size_t)(dev->speed_hz / 80000U) + 1U;
    if (len > sizeof(buffer)) {
        len = sizeof(buffer);
    }
    esp_err_t err = solar_os_bus_spi_transfer(dev->spi_bus, dev->cs_pin, 0,
                                              dev->speed_hz, buffer, NULL, len);
    if (err != ESP_OK) {
        return err;
    }
    return lr11xx_wait_busy(dev);
}

/* See LR11XX_REG_HIGH_ACP. Read-modify-write of one register bit. */
static esp_err_t lr11xx_clear_high_acp(lr11xx_t *dev)
{
    uint8_t params[12] = {0};
    lr11xx_put_be32(&params[0], LR11XX_REG_HIGH_ACP);
    lr11xx_put_be32(&params[4], LR11XX_REG_HIGH_ACP_BIT);
    return lr11xx_command(dev, LR11XX_OP_WRITE_REG_MEM32_MASK, params, sizeof(params));
}

static esp_err_t lr11xx_wake(lr11xx_t *dev)
{
    const esp_err_t err = lr11xx_poke(dev);
    return err == ESP_OK ? lr11xx_clear_high_acp(dev) : err;
}

static esp_err_t lr11xx_set_standby(lr11xx_t *dev)
{
    const uint8_t param = LR11XX_STANDBY_RC;
    return lr11xx_command(dev, LR11XX_OP_SET_STANDBY, &param, 1);
}

/*
 * After a failure the part may be in any mode, asleep included, and on any
 * mix of old and new settings. The driver stops claiming a mode or settings
 * for it, and remembers to bring it back by hand before it is next used.
 */
static void lr11xx_lose_track(lr11xx_t *dev)
{
    dev->state = SOLAR_OS_RADIO_STATE_UNKNOWN;
    dev->configured = false;
    dev->has_last_packet = false;
    dev->recover = true;
}

/* Brings a part the driver lost track of to standby, whatever it was doing:
 * the wake is harmless to one already awake. */
static esp_err_t lr11xx_recover(lr11xx_t *dev)
{
    esp_err_t err = lr11xx_poke(dev);
    if (err == ESP_OK) {
        err = lr11xx_set_standby(dev);
    }
    if (err == ESP_OK) {
        err = lr11xx_clear_high_acp(dev);
    }
    if (err == ESP_OK) {
        dev->recover = false;
    }
    return err;
}

/*
 * Before a query that may come at any time: a part asleep, or one the driver
 * lost track of, does not answer. Either is brought to standby and left
 * there, and one that will not come is lost track of.
 */
static esp_err_t lr11xx_rouse(lr11xx_t *dev)
{
    esp_err_t err = ESP_OK;
    if (dev->recover) {
        err = lr11xx_recover(dev);
    } else if (dev->state == SOLAR_OS_RADIO_STATE_SLEEP) {
        err = lr11xx_wake(dev);
    } else {
        return ESP_OK;
    }
    if (err == ESP_OK) {
        dev->state = SOLAR_OS_RADIO_STATE_STANDBY;
    } else {
        lr11xx_lose_track(dev);
    }
    return err;
}

static esp_err_t lr11xx_get_irq_status(lr11xx_t *dev, uint32_t *irq)
{
    /* stat1, stat2, then the 32-bit interrupt word, in one window. */
    uint8_t resp[6] = {0};
    const esp_err_t err = lr11xx_direct_read(dev, resp, sizeof(resp));
    if (err != ESP_OK) {
        return err;
    }
    /* All ones is every interrupt raised: a transmission that finished and
     * a packet that arrived, from a part that is not there. All zeros is no
     * interrupt at all, which a caller waiting for one takes for nothing
     * heard until its own timeout runs out; the mode is what gives it away,
     * since no part that answers a read is asleep. */
    if (LR11XX_STAT1_COMMAND(resp[0]) > LR11XX_COMMAND_LAST ||
        LR11XX_STAT2_MODE(resp[1]) == LR11XX_MODE_SLEEP ||
        LR11XX_STAT2_MODE(resp[1]) > LR11XX_MODE_LAST) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (irq != NULL) {
        *irq = ((uint32_t)resp[2] << 24) | ((uint32_t)resp[3] << 16) |
               ((uint32_t)resp[4] << 8) | (uint32_t)resp[5];
    }
    return ESP_OK;
}

static esp_err_t lr11xx_clear_irq(lr11xx_t *dev, uint32_t mask)
{
    uint8_t params[4];
    lr11xx_put_be32(params, mask);
    return lr11xx_command(dev, LR11XX_OP_CLEAR_IRQ, params, sizeof(params));
}

static esp_err_t lr11xx_set_dio_irq_params(lr11xx_t *dev, uint32_t irq1, uint32_t irq2)
{
    uint8_t params[8];
    lr11xx_put_be32(&params[0], irq1);
    lr11xx_put_be32(&params[4], irq2);
    return lr11xx_command(dev, LR11XX_OP_SET_DIO_IRQ_PARAMS, params, sizeof(params));
}

static esp_err_t lr11xx_write_buffer(lr11xx_t *dev, const uint8_t *data, size_t len)
{
    /* WriteBuffer8 always fills the transmit buffer from offset zero; there is
     * no offset parameter the way SX126x WriteBuffer has one. */
    if (len == 0 || len > LR11XX_MAX_PACKET_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    return lr11xx_command(dev, LR11XX_OP_WRITE_BUFFER8, data, len);
}

static esp_err_t lr11xx_read_buffer(lr11xx_t *dev, uint8_t offset, uint8_t *data, size_t len)
{
    const uint8_t params[2] = {offset, (uint8_t)len};
    if (len == 0 || len > LR11XX_MAX_PACKET_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    return lr11xx_query(dev, LR11XX_OP_READ_BUFFER8, params, sizeof(params), data, len);
}

esp_err_t lr11xx_init(lr11xx_t *dev,
                      const char *spi_bus,
                      int cs_pin,
                      int busy_pin,
                      int reset_pin,
                      int irq_pin,
                      uint32_t speed_hz,
                      uint16_t tcxo_mv,
                      const lr11xx_rf_switch_t *rf_switch)
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
    dev->tcxo_mv = tcxo_mv;
    if (rf_switch != NULL) {
        dev->rf_switch = *rf_switch;
        dev->has_rf_switch = true;
    }
    dev->part = LR11XX_PART_UNKNOWN;
    dev->state = SOLAR_OS_RADIO_STATE_UNKNOWN;
    /* Here rather than on first use, where two callers arriving together
     * would each make one. Its storage is the device's, so there is nothing
     * to free. */
    dev->mutex = xSemaphoreCreateMutexStatic(&dev->mutex_storage);
    if (dev->mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

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

    /* Chip select is driven high as a plain GPIO before the reset: the SPI
     * peripheral only owns it during a transfer, and on a shared bus a
     * floating NSS lets the part read another device's bytes as commands. */
    const gpio_config_t cs_config = {
        .pin_bit_mask = 1ULL << (uint32_t)cs_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&cs_config);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(cs_pin, 1);

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
        /* NRESET is active low for at least 100 us. The part then holds BUSY
         * high for its whole start-up, around 200 ms, so readiness is the
         * BUSY wait below rather than a fixed delay. */
        gpio_set_level(reset_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
        gpio_set_level(reset_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
        gpio_set_level(reset_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return lr11xx_wait_busy(dev);
}

esp_err_t lr11xx_probe(lr11xx_t *dev, lr11xx_version_t *version)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lr11xx_lock(dev);
    if (err != ESP_OK) {
        return err;
    }

    /* GetVersion answers with four bytes after the status byte: hardware
     * revision, device type, then the firmware major and minor. The device
     * type is how the driver learns which part it is talking to. */
    uint8_t resp[4] = {0};
    err = lr11xx_rouse(dev);
    if (err == ESP_OK) {
        err = lr11xx_query(dev, LR11XX_OP_GET_VERSION, NULL, 0, resp, sizeof(resp));
    }
    if (err != ESP_OK) {
        lr11xx_unlock(dev);
        return err;
    }

    const lr11xx_part_t part =
        (resp[1] >= LR11XX_PART_LR1110 && resp[1] <= LR11XX_PART_LR1121)
            ? (lr11xx_part_t)resp[1] : LR11XX_PART_UNKNOWN;
    /* Reported whatever the answer was, so a caller can say what it saw. */
    if (version != NULL) {
        version->part = part;
        version->hardware = resp[0];
        version->device_code = resp[1];
        version->firmware_major = resp[2];
        version->firmware_minor = resp[3];
    }
    if (part == LR11XX_PART_UNKNOWN) {
        /* An absent or dead module reads back all zeroes or all ones; an
         * unknown non-trivial type byte is a newer family member this driver
         * has not been taught about, and either way it is not safe to drive. */
        lr11xx_unlock(dev);
        return ESP_ERR_NOT_FOUND;
    }
    dev->part = part;

    lr11xx_unlock(dev);
    return ESP_OK;
}

/* The narrowest bandwidth the part offers that holds the one asked for, as
 * its code and as the bandwidth that code stands for. */
static bool lr11xx_lora_bandwidth(uint32_t bw_hz, uint8_t *code, uint32_t *actual_hz)
{
    /* The sub-GHz codes happen to match the SX126x ones, but the family adds
     * 203/406/812 kHz for the 2.4 GHz path and drops SX126x's 7.8 kHz, so the
     * table is written out rather than shared. */
    static const struct {
        uint32_t hz;
        uint8_t code;
    } widths[] = {
        {10400U, 0x08U}, {15600U, 0x01U}, {20800U, 0x09U}, {31250U, 0x02U},
        {41700U, 0x0AU}, {62500U, 0x03U}, {125000U, 0x04U}, {250000U, 0x05U},
        {500000U, 0x06U},
    };
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        if (bw_hz <= widths[i].hz) {
            if (code != NULL) {
                *code = widths[i].code;
            }
            if (actual_hz != NULL) {
                *actual_hz = widths[i].hz;
            }
            return true;
        }
    }
    return false;
}

static void lr11xx_encode_lora_modulation(const solar_os_radio_config_t *config, uint8_t out[4])
{
    const uint8_t sf = config->spreading_factor;
    uint8_t bw = 0x04U; /* 125 kHz */
    (void)lr11xx_lora_bandwidth(config->rx_bandwidth_hz, &bw, NULL);
    const uint8_t cr = (uint8_t)(config->coding_rate_denominator - 4U);
    /* Low-data-rate optimize is on wherever a symbol lasts longer than 16 ms.
     * Both ends have to agree, and this is the rule the modulation defines:
     * SF11 and SF12 at 125 kHz, SF12 at 250 kHz, and more as it narrows. */
    const uint64_t symbol_us = ((uint64_t)1U << sf) * 1000000ULL / config->rx_bandwidth_hz;
    const uint8_t ldro = symbol_us > 16000ULL ? 1U : 0U;
    out[0] = sf;
    out[1] = bw;
    out[2] = cr;
    out[3] = ldro;
}

static void lr11xx_encode_lora_packet(const solar_os_radio_config_t *config,
                                      uint16_t preamble,
                                      uint8_t payload_len,
                                      uint8_t out[6])
{
    out[0] = (uint8_t)(preamble >> 8);
    out[1] = (uint8_t)(preamble & 0xFFU);
    out[2] = config->variable_length ? 0x00U : 0x01U; /* explicit or implicit header */
    out[3] = payload_len;
    out[4] = config->crc_enabled ? 0x01U : 0x00U;
    out[5] = 0x00U; /* standard IQ */
}

/*
 * The payload length is part of the packet parameters, and the part reads it
 * two ways: as the length to transmit, and as the longest packet it will
 * accept while listening. A transmit therefore re-sends it for its own
 * payload and a receive puts the maximum back, or a radio that has just sent
 * a short packet is deaf to any longer one.
 *
 * The preamble length is read two ways as well: as the preamble to transmit,
 * and as the longest one to expect. Listening for the configured length, the
 * part misses a sender whose preamble is much longer, so `listening` asks for
 * the longest there is, which takes every sender.
 */
static esp_err_t lr11xx_set_packet_params(lr11xx_t *dev,
                                          const solar_os_radio_config_t *config,
                                          uint8_t payload_len,
                                          bool listening)
{
    uint8_t params[6];
    lr11xx_encode_lora_packet(config, listening ? UINT16_MAX : config->preamble_len,
                              payload_len, params);
    return lr11xx_command(dev, LR11XX_OP_SET_PACKET_PARAMS, params, sizeof(params));
}

/* Continuous receive, able to take the longest packet the configuration
 * allows whatever was transmitted last. */
static esp_err_t lr11xx_start_rx(lr11xx_t *dev)
{
    esp_err_t err = lr11xx_set_packet_params(dev, &dev->config,
                                             (uint8_t)dev->config.payload_length, true);
    if (err == ESP_OK) {
        err = lr11xx_clear_irq(dev, LR11XX_IRQ_ALL);
    }
    if (err == ESP_OK) {
        const uint8_t params[3] = {0xFFU, 0xFFU, 0xFFU}; /* no timeout */
        err = lr11xx_command(dev, LR11XX_OP_SET_RX, params, sizeof(params));
    }
    return err;
}

static esp_err_t lr11xx_set_tcxo(lr11xx_t *dev)
{
    /* SetTcxoMode powers an external TCXO from DIO3. A board with a crystal
     * must not see this command, so zero millivolts skips it. */
    uint8_t tune;
    if (dev->tcxo_mv >= 3300U) tune = 0x07U;
    else if (dev->tcxo_mv >= 3000U) tune = 0x06U;
    else if (dev->tcxo_mv >= 2700U) tune = 0x05U;
    else if (dev->tcxo_mv >= 2400U) tune = 0x04U;
    else if (dev->tcxo_mv >= 2200U) tune = 0x03U;
    else if (dev->tcxo_mv >= 1800U) tune = 0x02U;
    else if (dev->tcxo_mv >= 1700U) tune = 0x01U;
    else tune = 0x00U;

    /* 5 ms of start-up, in 1/32768 s ticks. The part holds BUSY for the whole
     * delay, so too large a figure looks like a radio that hangs. */
    const uint32_t delay_ticks = (uint32_t)(5000000ULL / LR11XX_TICK_NS);
    const uint8_t params[4] = {
        tune,
        (uint8_t)(delay_ticks >> 16), (uint8_t)(delay_ticks >> 8),
        (uint8_t)(delay_ticks & 0xFFU),
    };
    esp_err_t err = lr11xx_command(dev, LR11XX_OP_SET_TCXO_MODE, params, sizeof(params));
    if (err == ESP_OK) {
        /* Switching the clock source invalidates the factory calibration, so
         * the part has to be recalibrated and its error flags cleared before
         * anything else is configured. */
        const uint8_t calibrate = LR11XX_CALIBRATE_ALL;
        err = lr11xx_command(dev, LR11XX_OP_CALIBRATE, &calibrate, 1);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (err == ESP_OK) {
        err = lr11xx_command(dev, LR11XX_OP_CLEAR_ERRORS, NULL, 0);
    }
    return err;
}

/*
 * Semtech's reference operating points for the sub-GHz amplifiers, indexed by
 * requested dBm from -17 upward: the SetTxParams power, then paSel,
 * regPaSupply, paDutyCycle and paHpSel. This is measured tuning from the
 * LR1110 evaluation-shield support code, not a formula, and a wrong
 * high-power entry can push that amplifier outside its safe operating area.
 */
typedef struct {
    int8_t tx_power;
    uint8_t pa_sel;
    uint8_t supply;
    uint8_t duty_cycle;
    uint8_t hp_sel;
} lr11xx_pa_point_t;

#define LR11XX_PA_MIN_DBM (-17)
#define LR11XX_PA_MAX_DBM (22)

static const lr11xx_pa_point_t lr11xx_pa_points[] = {
    {-15, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U}, /* -17 */
    {-14, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {-13, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {-12, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {-11, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {-9, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {-8, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {-7, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {-6, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {-5, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {-4, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {-3, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {-2, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {-1, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {0, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {1, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {2, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {3, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U}, /* 0 dBm */
    {3, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x01U, 0x00U},
    {4, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x01U, 0x00U},
    {7, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {8, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {9, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {10, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {12, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {13, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {14, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x00U, 0x00U},
    {14, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x01U, 0x00U}, /* 10 dBm */
    {13, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x02U, 0x00U},
    {14, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x02U, 0x00U},
    {14, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x04U, 0x00U},
    {14, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x05U, 0x00U}, /* 14 dBm */
    {14, LR11XX_PA_SEL_LP, LR11XX_PA_REG_SUPPLY_VREG, 0x07U, 0x00U},
    {22, LR11XX_PA_SEL_HP, LR11XX_PA_REG_SUPPLY_VBAT, 0x03U, 0x03U}, /* 16 dBm */
    {22, LR11XX_PA_SEL_HP, LR11XX_PA_REG_SUPPLY_VBAT, 0x04U, 0x03U},
    {22, LR11XX_PA_SEL_HP, LR11XX_PA_REG_SUPPLY_VBAT, 0x02U, 0x05U},
    {22, LR11XX_PA_SEL_HP, LR11XX_PA_REG_SUPPLY_VBAT, 0x05U, 0x04U},
    {22, LR11XX_PA_SEL_HP, LR11XX_PA_REG_SUPPLY_VBAT, 0x03U, 0x07U}, /* 20 dBm */
    {21, LR11XX_PA_SEL_HP, LR11XX_PA_REG_SUPPLY_VBAT, 0x04U, 0x07U},
    {22, LR11XX_PA_SEL_HP, LR11XX_PA_REG_SUPPLY_VBAT, 0x04U, 0x07U}, /* 22 dBm */
};

_Static_assert(sizeof(lr11xx_pa_points) / sizeof(lr11xx_pa_points[0]) ==
                   LR11XX_PA_MAX_DBM - LR11XX_PA_MIN_DBM + 1,
               "one PA operating point per dBm");

static esp_err_t lr11xx_set_pa(lr11xx_t *dev, int8_t power)
{
    /* The low-power amplifier up to 15 dBm and the high-power one above it.
     * The high-frequency amplifier, the only path to 2.4 GHz, is never
     * selected: lr11xx_configure() refuses that band. */
    const lr11xx_pa_point_t *point = &lr11xx_pa_points[power - LR11XX_PA_MIN_DBM];

    const uint8_t pa_params[4] = {
        point->pa_sel, point->supply, point->duty_cycle, point->hp_sel,
    };
    esp_err_t err = lr11xx_command(dev, LR11XX_OP_SET_PA_CONFIG, pa_params, sizeof(pa_params));
    if (err == ESP_OK) {
        /* Ramp code 0x04 is 80 us on this family; the SX126x's identical code
         * means 200 us, so the number does not carry over. */
        const uint8_t params[2] = {(uint8_t)point->tx_power, 0x04U};
        err = lr11xx_command(dev, LR11XX_OP_SET_TX_PARAMS, params, sizeof(params));
    }
    return err;
}

/*
 * Fills in what the caller left at zero and refuses what the part cannot do,
 * so the settings kept afterwards are the ones in force rather than the ones
 * asked for.
 */
static esp_err_t lr11xx_resolve_config(const solar_os_radio_config_t *asked,
                                       solar_os_radio_config_t *config)
{
    *config = *asked;
    /* Above the sub-GHz front end the part needs the high-frequency amplifier
     * and a board that routes its port, and on an LR1110 there is no 2.4 GHz
     * transceiver behind it at all - only the Wi-Fi scanner. Refuse, rather
     * than configure a transmit that goes into the wrong front end. */
    if (config->frequency_hz < 150000000U || config->frequency_hz > 960000000U) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* LoRa is the one modulation this drives. The part has GFSK as well,
     * which is left out until it has been run against another radio. */
    if (config->modulation != SOLAR_OS_RADIO_MODULATION_LORA) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (config->preamble_len == 0U) {
        config->preamble_len = 8U;
    }
    if (config->sync_word_len == 0U) {
        config->sync_word_len = 1U;
        config->sync_word[0] = 0x12U;
    }
    /* The LoRa sync word is one byte, and SetLoRaSyncWord takes exactly that.
     * A longer one cannot be sent in part: the settings reported would say
     * two bytes while the part was given one. */
    if (config->sync_word_len != 1U) {
        return ESP_ERR_INVALID_ARG;
    }
    /* A length left at zero is the longest the part can carry, whatever the
     * header, as the RFM95 driver takes it. There is then one length in the
     * settings, the one the part is given; and under an implicit header,
     * where the receiver decodes the length it was configured with, a
     * listener and a sender that both say nothing agree. */
    if (config->payload_length == 0U) {
        config->payload_length = LR11XX_MAX_PACKET_LEN;
    }
    /* The payload length is one byte on the wire. */
    if (config->payload_length > LR11XX_MAX_PACKET_LEN ||
        config->tx_power_dbm < LR11XX_PA_MIN_DBM ||
        config->tx_power_dbm > LR11XX_PA_MAX_DBM) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->rx_bandwidth_hz == 0U) {
        config->rx_bandwidth_hz = 125000U;
    }
    if (config->spreading_factor == 0U) {
        config->spreading_factor = 7U;
    }
    if (config->coding_rate_denominator == 0U) {
        config->coding_rate_denominator = 5U;
    }
    /* Kept as the bandwidth in force, which is the next one up the part has. */
    if (!lr11xx_lora_bandwidth(config->rx_bandwidth_hz, NULL, &config->rx_bandwidth_hz) ||
        config->spreading_factor < 5U || config->spreading_factor > 12U ||
        config->coding_rate_denominator < 5U || config->coding_rate_denominator > 8U) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t lr11xx_configure(lr11xx_t *dev, const solar_os_radio_config_t *asked)
{
    if (dev == NULL || asked == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_radio_config_t resolved;
    esp_err_t err = lr11xx_resolve_config(asked, &resolved);
    if (err != ESP_OK) {
        return err;
    }
    const solar_os_radio_config_t *config = &resolved;

    err = lr11xx_lock(dev);
    if (err != ESP_OK) {
        return err;
    }

    if (dev->recover) {
        err = lr11xx_recover(dev);
    } else if (dev->state == SOLAR_OS_RADIO_STATE_SLEEP) {
        err = lr11xx_wake(dev);
    }
    if (err == ESP_OK) {
        err = lr11xx_set_standby(dev);
    }
    if (err == ESP_OK) {
        const uint8_t reg_mode = LR11XX_REG_MODE_DCDC;
        err = lr11xx_command(dev, LR11XX_OP_SET_REG_MODE, &reg_mode, 1);
    }
    if (err == ESP_OK && dev->tcxo_mv != 0U) {
        err = lr11xx_set_tcxo(dev);
    }
    if (err == ESP_OK && dev->has_rf_switch) {
        /* Sent from standby RC. Without a table every DIO stays in high
         * impedance, the part's own default. */
        const uint8_t params[8] = {
            dev->rf_switch.enable,
            dev->rf_switch.standby,
            dev->rf_switch.rx,
            dev->rf_switch.tx,
            dev->rf_switch.tx_hp,
            dev->rf_switch.tx_hf,
            dev->rf_switch.gnss,
            dev->rf_switch.wifi,
        };
        err = lr11xx_command(dev, LR11XX_OP_SET_DIO_AS_RF_SWITCH, params, sizeof(params));
    }
    if (err == ESP_OK) {
        const uint8_t packet_type = LR11XX_PACKET_TYPE_LORA;
        err = lr11xx_command(dev, LR11XX_OP_SET_PACKET_TYPE, &packet_type, 1);
    }
    if (err == ESP_OK) {
        /* Plain big-endian hertz, not the SX126x's PLL steps. */
        uint8_t params[4];
        lr11xx_put_be32(params, config->frequency_hz);
        err = lr11xx_command(dev, LR11XX_OP_SET_RF_FREQUENCY, params, sizeof(params));
    }
    if (err == ESP_OK) {
        /* CalibImage takes the band as two bounds in 4 MHz steps; the lower
         * floors and the upper ceils so the interval holds the frequency. */
        const uint32_t mhz = config->frequency_hz / 1000000U;
        const uint8_t params[2] = {
            (uint8_t)(mhz / 4U),
            (uint8_t)((mhz + 3U) / 4U),
        };
        err = lr11xx_command(dev, LR11XX_OP_CALIBRATE_IMAGE, params, sizeof(params));
    }
    if (err == ESP_OK) {
        uint8_t params[4];
        lr11xx_encode_lora_modulation(config, params);
        err = lr11xx_command(dev, LR11XX_OP_SET_MODULATION_PARAMS, params, sizeof(params));
    }
    if (err == ESP_OK) {
        err = lr11xx_set_packet_params(dev, config, (uint8_t)config->payload_length, false);
    }
    if (err == ESP_OK) {
        err = lr11xx_set_pa(dev, config->tx_power_dbm);
    }
    if (err == ESP_OK) {
        /* One command, one byte - the SX126x's split nibble-mapped sync-word
         * registers have no counterpart here. */
        err = lr11xx_command(dev, LR11XX_OP_SET_LORA_SYNC_WORD, &config->sync_word[0], 1);
    }
    if (err == ESP_OK) {
        const uint32_t irq_mask = LR11XX_IRQ_TX_DONE | LR11XX_IRQ_RX_DONE |
            LR11XX_IRQ_TIMEOUT | LR11XX_IRQ_CRC_ERROR | LR11XX_IRQ_HEADER_ERROR;
        /* Only IRQ1 is armed: every board that carries this part wires a
         * single interrupt line. */
        err = lr11xx_set_dio_irq_params(dev, irq_mask, 0UL);
    }
    if (err == ESP_OK) {
        err = lr11xx_clear_irq(dev, LR11XX_IRQ_ALL);
    }
    /* What the last packet measured was measured under the old settings. */
    dev->has_last_packet = false;
    if (err == ESP_OK) {
        dev->config = *config;
        dev->configured = true;
        dev->state = SOLAR_OS_RADIO_STATE_STANDBY;
    } else {
        lr11xx_lose_track(dev);
    }

    lr11xx_unlock(dev);
    return err;
}

esp_err_t lr11xx_set_state(lr11xx_t *dev, solar_os_radio_state_t state)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lr11xx_lock(dev);
    if (err != ESP_OK) {
        return err;
    }
    /* Listening and transmitting use the settings, so there have to be some. */
    if (!dev->configured &&
        (state == SOLAR_OS_RADIO_STATE_RX || state == SOLAR_OS_RADIO_STATE_TX)) {
        lr11xx_unlock(dev);
        return ESP_ERR_INVALID_STATE;
    }
    if (state != SOLAR_OS_RADIO_STATE_SLEEP && state != SOLAR_OS_RADIO_STATE_STANDBY &&
        state != SOLAR_OS_RADIO_STATE_RX && state != SOLAR_OS_RADIO_STATE_TX) {
        lr11xx_unlock(dev);
        return ESP_ERR_INVALID_ARG;
    }
    if (dev->recover) {
        err = lr11xx_recover(dev);
    } else if (dev->state == SOLAR_OS_RADIO_STATE_SLEEP) {
        if (state == SOLAR_OS_RADIO_STATE_SLEEP) {
            /* A sleeping part holds BUSY high and takes no command. */
            lr11xx_unlock(dev);
            return ESP_OK;
        }
        err = lr11xx_wake(dev);
    }

    if (err == ESP_OK) {
        switch (state) {
        case SOLAR_OS_RADIO_STATE_SLEEP: {
            /* Warm start, no RTC wake-up. A cold sleep would lose the whole
             * configuration, and nothing here reconfigures on wake. */
            const uint8_t params[5] = {LR11XX_SLEEP_WARM_START, 0x00U, 0x00U, 0x00U, 0x00U};
            err = lr11xx_write(dev, LR11XX_OP_SET_SLEEP, params, sizeof(params));
            break;
        }
        case SOLAR_OS_RADIO_STATE_STANDBY:
            err = lr11xx_set_standby(dev);
            break;
        case SOLAR_OS_RADIO_STATE_RX:
            err = lr11xx_start_rx(dev);
            break;
        case SOLAR_OS_RADIO_STATE_TX: {
            const uint8_t params[3] = {0x00U, 0x00U, 0x00U}; /* no hardware timeout */
            err = lr11xx_command(dev, LR11XX_OP_SET_TX, params, sizeof(params));
            break;
        }
        case SOLAR_OS_RADIO_STATE_UNKNOWN:
        default:
            break;
        }
    }
    if (err == ESP_OK) {
        dev->state = state;
        if (state == SOLAR_OS_RADIO_STATE_SLEEP) {
            dev->has_last_packet = false;
        }
    } else {
        lr11xx_lose_track(dev);
    }

    lr11xx_unlock(dev);
    return err;
}

esp_err_t lr11xx_get_errors(lr11xx_t *dev, uint16_t *errors)
{
    if (dev == NULL || errors == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lr11xx_lock(dev);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t resp[2] = {0};
    err = lr11xx_rouse(dev);
    if (err == ESP_OK) {
        err = lr11xx_query(dev, LR11XX_OP_GET_ERRORS, NULL, 0, resp, sizeof(resp));
    }
    if (err == ESP_OK) {
        *errors = (uint16_t)(((uint16_t)resp[0] << 8) | resp[1]);
    }
    lr11xx_unlock(dev);
    return err;
}

esp_err_t lr11xx_get_status(lr11xx_t *dev, solar_os_radio_status_t *status)
{
    if (dev == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lr11xx_lock(dev);
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
    } else if (dev->state == SOLAR_OS_RADIO_STATE_RX) {
        /* Listening with nothing heard yet: the level on the channel right
         * now, which says the receiver is alive even when nobody is
         * transmitting. Same -raw/2 dBm scaling as a packet's RSSI. */
        uint8_t raw = 0;
        if (lr11xx_query(dev, LR11XX_OP_GET_RSSI_INST, NULL, 0, &raw, 1) == ESP_OK) {
            status->has_rssi = true;
            status->rssi_dbm = (int16_t)(-(int16_t)(raw >> 1));
        } else {
            /* It was listening when last heard from, and is not answering. */
            lr11xx_lose_track(dev);
            status->state = dev->state;
        }
    }
    lr11xx_unlock(dev);
    return ESP_OK;
}

esp_err_t lr11xx_send(lr11xx_t *dev, const solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    /* No time to send in is no send: the transmission would start and be
     * cut off at the first look for its end. */
    if (dev == NULL || packet == NULL || packet->len == 0 ||
        packet->len > LR11XX_MAX_PACKET_LEN || timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lr11xx_lock(dev);
    if (err != ESP_OK) {
        return err;
    }
    if (!dev->configured) {
        lr11xx_unlock(dev);
        return ESP_ERR_INVALID_STATE;
    }
    /* The packet parameters are re-sent for this packet's length, which is
     * what the part transmits. With an implicit header the receiver cannot
     * learn that length from the air and decodes the one it was configured
     * with, so a packet of any other length is refused before the part is
     * touched, as the RFM95 driver refuses it. */
    if (!dev->config.variable_length && packet->len != dev->config.payload_length) {
        lr11xx_unlock(dev);
        return ESP_ERR_INVALID_SIZE;
    }
    if (dev->state == SOLAR_OS_RADIO_STATE_SLEEP) {
        err = lr11xx_wake(dev);
        if (err != ESP_OK) {
            lr11xx_lose_track(dev);
            lr11xx_unlock(dev);
            return err;
        }
    }

    if (err == ESP_OK) {
        err = lr11xx_set_packet_params(dev, &dev->config, (uint8_t)packet->len, false);
    }
    if (err == ESP_OK) {
        err = lr11xx_clear_irq(dev, LR11XX_IRQ_ALL);
    }
    if (err == ESP_OK) {
        err = lr11xx_write_buffer(dev, packet->data, packet->len);
    }
    if (err == ESP_OK) {
        const uint8_t params[3] = {0x00U, 0x00U, 0x00U};
        err = lr11xx_command(dev, LR11XX_OP_SET_TX, params, sizeof(params));
    }
    if (err == ESP_OK) {
        dev->state = SOLAR_OS_RADIO_STATE_TX;
        const int64_t start = esp_timer_get_time();
        while (true) {
            uint32_t irq = 0UL;
            err = lr11xx_get_irq_status(dev, &irq);
            if (err != ESP_OK) {
                break;
            }
            if (irq & LR11XX_IRQ_TX_DONE) {
                /* The packet went out; a clear the part then refuses is
                 * still a part no longer answering, and the caller hears
                 * that rather than that all went well. */
                err = lr11xx_clear_irq(dev, LR11XX_IRQ_ALL);
                break;
            }
            if (irq & LR11XX_IRQ_TIMEOUT) {
                err = lr11xx_clear_irq(dev, LR11XX_IRQ_ALL);
                if (err == ESP_OK) {
                    err = ESP_ERR_TIMEOUT;
                }
                break;
            }
            if ((uint32_t)((esp_timer_get_time() - start) / 1000) >= timeout_ms) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(LR11XX_POLL_INTERVAL_MS));
        }
    }
    /* Standby on every path, sent or not: a part left listening after a send
     * that failed partway would take nothing longer than that packet. */
    const esp_err_t standby = lr11xx_set_standby(dev);
    if (standby == ESP_OK) {
        dev->state = SOLAR_OS_RADIO_STATE_STANDBY;
    } else {
        lr11xx_lose_track(dev);
        if (err == ESP_OK) {
            err = standby;
        }
    }
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        /* A command refused or unanswered on the way is the part not
         * answering, whether or not it then took the standby: it is no
         * longer known to be on these settings. The caller's timeout and
         * the part's own are a transmission not finished, nothing more. */
        lr11xx_lose_track(dev);
    }

    lr11xx_unlock(dev);
    return err;
}

esp_err_t lr11xx_send_stream(lr11xx_t *dev, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (data == NULL || len == 0 || len > LR11XX_MAX_PACKET_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_radio_packet_t packet = {0};
    packet.len = len;
    memcpy(packet.data, data, len);
    return lr11xx_send(dev, &packet, timeout_ms);
}

esp_err_t lr11xx_receive(lr11xx_t *dev, solar_os_radio_packet_t *packet, uint32_t timeout_ms)
{
    if (dev == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lr11xx_lock(dev);
    if (err != ESP_OK) {
        return err;
    }
    memset(packet, 0, sizeof(*packet));
    if (!dev->configured) {
        lr11xx_unlock(dev);
        return ESP_ERR_INVALID_STATE;
    }

    /* Same poll-not-session contract as sx1262_receive(): callers ask
     * repeatedly, usually with timeout_ms == 0, so the radio is armed once and
     * left in continuous receive. Re-issuing SetRx on every poll would leave
     * the part restarting its receiver instead of listening. */
    if (dev->state != SOLAR_OS_RADIO_STATE_RX) {
        if (dev->state == SOLAR_OS_RADIO_STATE_SLEEP) {
            err = lr11xx_wake(dev);
        }
        if (err == ESP_OK) {
            err = lr11xx_start_rx(dev);
        }
        if (err != ESP_OK) {
            lr11xx_lose_track(dev);
            lr11xx_unlock(dev);
            return err;
        }
        dev->state = SOLAR_OS_RADIO_STATE_RX;
    }

    const int64_t start = esp_timer_get_time();
    bool rx_done = false;
    while (true) {
        uint32_t irq = 0UL;
        err = lr11xx_get_irq_status(dev, &irq);
        if (err != ESP_OK) {
            /* A part that stops answering is no longer known to be
             * listening, and nothing heard is not what happened. */
            lr11xx_lose_track(dev);
            break;
        }
        if (irq & LR11XX_IRQ_RX_DONE) {
            /* Cleared before the buffer is read, so a packet arriving during
             * the read raises the flag again for the next poll. A clear that
             * fails leaves the flag up, and the next poll would hand back
             * this same packet a second time; that is a part not answering,
             * not a packet heard. */
            err = lr11xx_clear_irq(dev, LR11XX_IRQ_ALL);
            if (err != ESP_OK) {
                lr11xx_lose_track(dev);
                break;
            }
            rx_done = true;
            packet->crc_ok = (irq & LR11XX_IRQ_CRC_ERROR) == 0UL;
            break;
        }
        if ((uint32_t)((esp_timer_get_time() - start) / 1000) >= timeout_ms) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(LR11XX_POLL_INTERVAL_MS));
    }
    /* No SetStandby on any path: the part stays in continuous receive, ready
     * for the next poll, exactly as it was on the way in. */

    if (rx_done && err == ESP_OK) {
        uint8_t buffer_status[2] = {0};
        err = lr11xx_query(dev, LR11XX_OP_GET_RX_BUFFER_STATUS, NULL, 0,
                           buffer_status, sizeof(buffer_status));
        size_t payload_len = 0;
        if (err == ESP_OK) {
            payload_len = buffer_status[0];
            const uint8_t start_ptr = buffer_status[1];
            if (payload_len > SOLAR_OS_RADIO_PACKET_MAX) {
                payload_len = SOLAR_OS_RADIO_PACKET_MAX;
            }
            if (payload_len == 0) {
                err = ESP_ERR_INVALID_RESPONSE;
            } else {
                err = lr11xx_read_buffer(dev, start_ptr, packet->data, payload_len);
            }
        }
        if (err == ESP_OK) {
            packet->len = payload_len;
            /* Packet RSSI is -raw/2 dBm. SNR is a signed byte in quarter
             * decibels, rounded to nearest. */
            uint8_t pkt_status[3] = {0};
            if (lr11xx_query(dev, LR11XX_OP_GET_PACKET_STATUS, NULL, 0,
                             pkt_status, sizeof(pkt_status)) == ESP_OK) {
                packet->has_rssi = true;
                packet->rssi_dbm = (int16_t)(-(int8_t)(pkt_status[0] >> 1));
                dev->last_rssi_dbm = packet->rssi_dbm;
                packet->has_snr = true;
                packet->snr_db = (int16_t)((((int8_t)pkt_status[1]) + 2) >> 2);
                dev->last_snr_db = packet->snr_db;
                dev->has_last_packet = true;
            }
        }
    }
    if (rx_done && err != ESP_OK) {
        /* The flag is down and the packet could not be read out. A part that
         * says it heard something and will not say what is no better known
         * to be listening than one that answers no poll. */
        lr11xx_lose_track(dev);
    }

    lr11xx_unlock(dev);
    return err;
}
