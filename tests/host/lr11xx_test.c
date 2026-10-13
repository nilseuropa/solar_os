#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "lr11xx.h"

/*
 * What is worth testing without a radio is the wire format, because that is
 * exactly where the LR11xx differs from the SX1262 this codebase already
 * drives: two-byte opcodes, frequency in plain hertz, a payload written with
 * no offset byte, a one-byte LoRa sync word, and a 32-bit interrupt word. A
 * fake chip records every chip-select window so each of those can be asserted
 * against the bytes that would actually leave the bus.
 */

#define LR11XX_OP_GET_VERSION 0x0101U
#define LR11XX_OP_GET_ERRORS 0x010DU
#define LR11XX_OP_GET_RSSI_INST 0x0205U
#define LR11XX_OP_CALIBRATE 0x010FU
#define LR11XX_OP_SET_REG_MODE 0x0110U
#define LR11XX_OP_CALIBRATE_IMAGE 0x0111U
#define LR11XX_OP_SET_DIO_AS_RF_SWITCH 0x0112U
#define LR11XX_OP_SET_DIO_IRQ_PARAMS 0x0113U
#define LR11XX_OP_CLEAR_IRQ 0x0114U
#define LR11XX_OP_SET_TCXO_MODE 0x0117U
#define LR11XX_OP_SET_STANDBY 0x011CU
#define LR11XX_OP_WRITE_BUFFER8 0x0109U
#define LR11XX_OP_WRITE_REG_MEM32_MASK 0x010CU
#define LR11XX_OP_SET_SLEEP 0x011BU
#define LR11XX_OP_READ_BUFFER8 0x010AU
#define LR11XX_OP_GET_RX_BUFFER_STATUS 0x0203U
#define LR11XX_OP_GET_PACKET_STATUS 0x0204U
#define LR11XX_OP_SET_RX 0x0209U
#define LR11XX_OP_SET_TX 0x020AU
#define LR11XX_OP_SET_RF_FREQUENCY 0x020BU
#define LR11XX_OP_SET_PACKET_TYPE 0x020EU
#define LR11XX_OP_SET_MODULATION_PARAMS 0x020FU
#define LR11XX_OP_SET_PACKET_PARAMS 0x0210U
#define LR11XX_OP_SET_TX_PARAMS 0x0211U
#define LR11XX_OP_SET_PA_CONFIG 0x0215U
#define LR11XX_OP_SET_LORA_SYNC_WORD 0x022BU

#define IRQ_TX_DONE 0x00000004UL
#define IRQ_RX_DONE 0x00000008UL

#define MAX_WINDOWS 96
#define MAX_PARAMS 300

typedef struct {
    uint16_t opcode;
    uint8_t params[MAX_PARAMS];
    size_t param_len;
} window_t;

typedef struct {
    window_t windows[MAX_WINDOWS];
    size_t count;
    uint8_t pending[MAX_PARAMS];
    size_t pending_len;
    /* Scripted answers. */
    uint8_t version[4];
    uint32_t irq;
    uint8_t rx_payload[LR11XX_MAX_PACKET_LEN];
    uint8_t rx_len;
    uint8_t rx_start;
    uint8_t rssi_raw;
    int8_t snr_raw;
    unsigned wakes;
    size_t wake_bytes;
    unsigned reads;
    unsigned status_reads;
    uint8_t errors[2];
    uint8_t rssi_inst_raw;
    uint8_t stat1;
    /* What the part was last told, so a test can hold the driver's account
     * of the radio against the radio's own. */
    solar_os_radio_state_t mode;
    uint8_t max_len;
    /* Faults. fail_transfer counts down to the transfer that fails; dead
     * leaves every read as dead_byte, as an absent part does; reject_opcode
     * is one command the part answers with a parameter error. */
    unsigned fail_transfer;
    unsigned transfers;
    bool dead;
    uint8_t dead_byte;
    uint16_t reject_opcode;
    bool rejected;
} fake_chip_t;

static fake_chip_t chip;
static int64_t fake_time_us;
static int busy_level;

/* --- host stubs ---------------------------------------------------------- */

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "err";
}

int64_t esp_timer_get_time(void)
{
    /* Every call advances the clock so the driver's poll loops terminate. */
    fake_time_us += 1000;
    return fake_time_us;
}

static unsigned spins;
static unsigned sleeps;

void esp_rom_delay_us(uint32_t us)
{
    (void)us;
    spins++;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    sleeps++;
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)(fake_time_us / 1000);
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    (void)config;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t pin)
{
    (void)pin;
    return busy_level;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    (void)pin;
    (void)level;
    return ESP_OK;
}

/* --- the fake chip ------------------------------------------------------- */

static void queue(const uint8_t *data, size_t len)
{
    memcpy(chip.pending, data, len);
    chip.pending_len = len;
}

static void record_command(const uint8_t *tx, size_t len)
{
    assert(chip.count < MAX_WINDOWS);
    window_t *window = &chip.windows[chip.count++];
    window->opcode = (uint16_t)((tx[0] << 8) | tx[1]);
    window->param_len = len - 2U;
    assert(window->param_len <= MAX_PARAMS);
    memcpy(window->params, &tx[2], window->param_len);

    chip.rejected = chip.reject_opcode != 0U && window->opcode == chip.reject_opcode;
    if (!chip.rejected) {
        switch (window->opcode) {
        case LR11XX_OP_SET_STANDBY: chip.mode = SOLAR_OS_RADIO_STATE_STANDBY; break;
        case LR11XX_OP_SET_RX: chip.mode = SOLAR_OS_RADIO_STATE_RX; break;
        case LR11XX_OP_SET_TX: chip.mode = SOLAR_OS_RADIO_STATE_TX; break;
        case LR11XX_OP_SET_SLEEP: chip.mode = SOLAR_OS_RADIO_STATE_SLEEP; break;
        case LR11XX_OP_SET_PACKET_PARAMS: chip.max_len = window->params[3]; break;
        default: break;
        }
    }

    switch (window->opcode) {
    case LR11XX_OP_GET_VERSION:
        queue(chip.version, sizeof(chip.version));
        break;
    case LR11XX_OP_GET_ERRORS:
        queue(chip.errors, sizeof(chip.errors));
        break;
    case LR11XX_OP_GET_RSSI_INST:
        queue(&chip.rssi_inst_raw, 1U);
        break;
    case LR11XX_OP_GET_RX_BUFFER_STATUS: {
        const uint8_t status[2] = {chip.rx_len, chip.rx_start};
        queue(status, sizeof(status));
        break;
    }
    case LR11XX_OP_GET_PACKET_STATUS: {
        const uint8_t status[3] = {chip.rssi_raw, (uint8_t)chip.snr_raw, chip.rssi_raw};
        queue(status, sizeof(status));
        break;
    }
    case LR11XX_OP_READ_BUFFER8: {
        const uint8_t offset = window->params[0];
        const uint8_t length = window->params[1];
        assert((size_t)offset + length <= sizeof(chip.rx_payload));
        queue(&chip.rx_payload[offset], length);
        break;
    }
    default:
        chip.pending_len = 0;
        break;
    }
}

/* The mode the part reports in stat2, as the datasheet codes it. A part that
 * answers at all is never asleep: that one holds BUSY and answers nothing. */
static uint8_t chip_mode_code(void)
{
    switch (chip.mode) {
    case SOLAR_OS_RADIO_STATE_RX: return 0x04U;
    case SOLAR_OS_RADIO_STATE_TX: return 0x05U;
    case SOLAR_OS_RADIO_STATE_SLEEP: return 0x00U;
    case SOLAR_OS_RADIO_STATE_STANDBY:
    case SOLAR_OS_RADIO_STATE_UNKNOWN:
    default: return 0x01U; /* STBY_RC, as after a reset */
    }
}

esp_err_t solar_os_bus_spi_transfer(const char *name,
                                    int cs_pin,
                                    uint8_t mode,
                                    uint32_t speed_hz,
                                    const uint8_t *tx_data,
                                    uint8_t *rx_data,
                                    size_t len)
{
    assert(strcmp(name, "spi0") == 0);
    assert(cs_pin == 39);
    assert(mode == 0);
    assert(speed_hz == 2000000U);
    assert(len > 0);

    chip.transfers++;
    if (chip.fail_transfer > 0U && --chip.fail_transfer == 0U) {
        return ESP_FAIL;
    }
    if (chip.dead) {
        if (rx_data != NULL) {
            memset(rx_data, chip.dead_byte, len);
        }
        return ESP_OK;
    }

    /* A command window opens with a non-zero group byte; the LR11xx groups in
     * use are 0x01 and 0x02. Anything else is a response read or the one-byte
     * poke that wakes the part. */
    if (len >= 2U && tx_data != NULL && tx_data[0] != 0x00U) {
        record_command(tx_data, len);
        if (rx_data != NULL) {
            memset(rx_data, 0, len);
        }
        return ESP_OK;
    }

    /* The part reads MOSI during every read window, so a non-zero byte here
     * would be taken as the start of a command. Only 0x00 - the NOP opcode -
     * is safe. */
    for (size_t i = 0; i < len; i++) {
        assert(tx_data == NULL || tx_data[i] == 0x00U);
    }

    if (rx_data == NULL) {
        /* The wake poke: chip select held low with nothing but NOPs. */
        chip.wakes++;
        chip.wake_bytes = len;
        if (chip.mode == SOLAR_OS_RADIO_STATE_SLEEP) {
            chip.mode = SOLAR_OS_RADIO_STATE_STANDBY;
        }
        return ESP_OK;
    }
    chip.reads++;
    memset(rx_data, 0, len);
    if (chip.pending_len > 0 && len > 1U) {
        /* The second window of a command that returns data: one discarded
         * byte, then the answer. */
        const size_t copy = (chip.pending_len < len - 1U) ? chip.pending_len : len - 1U;
        rx_data[0] = chip.stat1;
        memcpy(&rx_data[1], chip.pending, copy);
        chip.pending_len = 0;
    } else if (len > 1U) {
        /* A bare read with nothing pending: the part volunteers stat1, stat2
         * and the 32-bit interrupt word. This is how status is fetched - there
         * is no GetIrqStatus opcode to send. */
        const uint8_t status[6] = {
            (uint8_t)((chip.rejected ? 0x01U : 0x02U) << 1),
            (uint8_t)(chip_mode_code() << 1),
            (uint8_t)(chip.irq >> 24), (uint8_t)(chip.irq >> 16),
            (uint8_t)(chip.irq >> 8), (uint8_t)(chip.irq & 0xFFU),
        };
        const size_t copy = (len < sizeof(status)) ? len : sizeof(status);
        memcpy(rx_data, status, copy);
        chip.status_reads++;
    }
    return ESP_OK;
}

/* --- helpers ------------------------------------------------------------- */

static void reset_chip(void)
{
    memset(&chip, 0, sizeof(chip));
    chip.version[0] = 0x22U; /* hardware revision */
    chip.version[1] = 0x01U; /* LR1110 */
    chip.version[2] = 0x01U;
    chip.version[3] = 0x0AU;
    chip.stat1 = 0x06U; /* command status: data ready */
    fake_time_us = 0;
    busy_level = 0;
}

static const window_t *find_window(uint16_t opcode)
{
    for (size_t i = 0; i < chip.count; i++) {
        if (chip.windows[i].opcode == opcode) {
            return &chip.windows[i];
        }
    }
    return NULL;
}

static size_t count_windows(uint16_t opcode)
{
    size_t total = 0;
    for (size_t i = 0; i < chip.count; i++) {
        if (chip.windows[i].opcode == opcode) {
            total++;
        }
    }
    return total;
}

static size_t index_of(uint16_t opcode)
{
    for (size_t i = 0; i < chip.count; i++) {
        if (chip.windows[i].opcode == opcode) {
            return i;
        }
    }
    assert(false);
    return 0;
}

static size_t last_index_of(uint16_t opcode)
{
    size_t found = chip.count;
    for (size_t i = 0; i < chip.count; i++) {
        if (chip.windows[i].opcode == opcode) {
            found = i;
        }
    }
    assert(found < chip.count);
    return found;
}

static solar_os_radio_config_t lora_config(void)
{
    solar_os_radio_config_t config = {0};
    config.frequency_hz = 915000000U;
    config.modulation = SOLAR_OS_RADIO_MODULATION_LORA;
    config.rx_bandwidth_hz = 125000U;
    config.spreading_factor = 7U;
    config.coding_rate_denominator = 5U;
    config.preamble_len = 8U;
    config.sync_word_len = 1U;
    config.sync_word[0] = 0x12U;
    config.tx_power_dbm = 14;
    config.crc_enabled = true;
    config.variable_length = true;
    config.payload_length = LR11XX_MAX_PACKET_LEN;
    return config;
}

/* One board's table: receive on DIO5 alone, transmit on both. Another board
 * differs, which is why the board declares it and the driver assumes none. */
static const lr11xx_rf_switch_t board_rf_switch = {
    .enable = LR11XX_RF_SWITCH_DIO5 | LR11XX_RF_SWITCH_DIO6,
    .standby = 0,
    .rx = LR11XX_RF_SWITCH_DIO5,
    .tx = LR11XX_RF_SWITCH_DIO5 | LR11XX_RF_SWITCH_DIO6,
    .tx_hp = LR11XX_RF_SWITCH_DIO6,
    .tx_hf = 0,
    .gnss = 0,
    .wifi = 0,
};

static esp_err_t open_device(lr11xx_t *dev, uint16_t tcxo_mv)
{
    return lr11xx_init(dev, "spi0", 39, 41, 45, 42, 2000000U, tcxo_mv, NULL);
}

static esp_err_t open_device_with_switch(lr11xx_t *dev)
{
    return lr11xx_init(dev, "spi0", 39, 41, 45, 42, 2000000U, 3300U, &board_rf_switch);
}

/* --- tests --------------------------------------------------------------- */

static const window_t *configure_and_find(const solar_os_radio_config_t *config, uint16_t opcode)
{
    static lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    assert(lr11xx_configure(&dev, config) == ESP_OK);
    const window_t *window = find_window(opcode);
    assert(window != NULL);
    return window;
}

static void test_lora_encoding(void)
{
    /* Frequency is plain big-endian hertz. Encoding 915 MHz in the SX1262's
     * PLL steps would send 0x39300000 and land in the wrong band. */
    solar_os_radio_config_t config = lora_config();
    const window_t *window = configure_and_find(&config, LR11XX_OP_SET_RF_FREQUENCY);
    assert(window->param_len == 4U);
    assert(memcmp(window->params, (const uint8_t[]){0x36U, 0x89U, 0xCAU, 0xC0U}, 4U) == 0);

    /* SF7, 125 kHz, CR 4/5, no low-data-rate optimize. */
    window = configure_and_find(&config, LR11XX_OP_SET_MODULATION_PARAMS);
    assert(memcmp(window->params, (const uint8_t[]){7U, 0x04U, 1U, 0U}, 4U) == 0);

    const struct {
        uint32_t bw_hz;
        uint8_t code;
    } bandwidths[] = {
        {250000U, 0x05U}, {500000U, 0x06U}, {62500U, 0x03U}, {10400U, 0x08U},
    };
    for (size_t i = 0; i < sizeof(bandwidths) / sizeof(bandwidths[0]); i++) {
        config.rx_bandwidth_hz = bandwidths[i].bw_hz;
        window = configure_and_find(&config, LR11XX_OP_SET_MODULATION_PARAMS);
        assert(window->params[1] == bandwidths[i].code);
    }
    /* Still at 10.4 kHz: a long symbol turns low-data-rate optimize on. */
    config.spreading_factor = 12U;
    window = configure_and_find(&config, LR11XX_OP_SET_MODULATION_PARAMS);
    assert(window->params[3] == 1U);

    /* Left at zero they take the defaults, which are then the settings
     * reported; out of range they are refused, not swapped for something
     * else. */
    config = lora_config();
    config.spreading_factor = 0U;
    config.coding_rate_denominator = 0U;
    window = configure_and_find(&config, LR11XX_OP_SET_MODULATION_PARAMS);
    assert(window->params[0] == 7U);
    assert(window->params[2] == 1U);
    {
        lr11xx_t dev;
        reset_chip();
        assert(open_device(&dev, 0) == ESP_OK);
        assert(lr11xx_configure(&dev, &config) == ESP_OK);
        solar_os_radio_status_t status;
        assert(lr11xx_get_status(&dev, &status) == ESP_OK);
        assert(status.config.spreading_factor == 7U);
        assert(status.config.coding_rate_denominator == 5U);
        config = lora_config();
        config.spreading_factor = 30U;
        assert(lr11xx_configure(&dev, &config) == ESP_ERR_INVALID_ARG);
        config = lora_config();
        config.coding_rate_denominator = 99U;
        assert(lr11xx_configure(&dev, &config) == ESP_ERR_INVALID_ARG);
        config = lora_config();
        config.tx_power_dbm = 23;
        assert(lr11xx_configure(&dev, &config) == ESP_ERR_INVALID_ARG);
        config.tx_power_dbm = -18;
        assert(lr11xx_configure(&dev, &config) == ESP_ERR_INVALID_ARG);
    }

    /* 16-bit preamble, explicit header, payload length, CRC on, standard IQ. */
    config = lora_config();
    config.preamble_len = 300U;
    config.payload_length = 64U;
    window = configure_and_find(&config, LR11XX_OP_SET_PACKET_PARAMS);
    assert(window->param_len == 6U);
    assert(memcmp(window->params, (const uint8_t[]){0x01U, 0x2CU, 0x00U, 64U, 0x01U, 0x00U}, 6U) == 0);

    config.variable_length = false;
    config.crc_enabled = false;
    window = configure_and_find(&config, LR11XX_OP_SET_PACKET_PARAMS);
    assert(window->params[2] == 0x01U);
    assert(window->params[4] == 0x00U);
}

static void test_config_limits(void)
{
    /* A bandwidth the part cannot reach, a payload length that does not fit
     * its one byte, and a frequency under the part's range are refused before
     * anything is sent. */
    lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    solar_os_radio_config_t config = lora_config();
    config.rx_bandwidth_hz = 600000U;
    assert(lr11xx_configure(&dev, &config) == ESP_ERR_INVALID_ARG);
    config = lora_config();
    config.payload_length = 256U;
    assert(lr11xx_configure(&dev, &config) == ESP_ERR_INVALID_ARG);
    config = lora_config();
    /* LoRa only: the part's GFSK has not been run against another radio. */
    config.modulation = SOLAR_OS_RADIO_MODULATION_GFSK;
    assert(lr11xx_configure(&dev, &config) == ESP_ERR_NOT_SUPPORTED);
    config = lora_config();
    config.frequency_hz = 100000000U;
    assert(lr11xx_configure(&dev, &config) == ESP_ERR_NOT_SUPPORTED);
    /* A LoRa sync word is one byte. A longer one is refused rather than cut
     * to its first byte and reported whole. */
    config = lora_config();
    config.sync_word_len = 2U;
    config.sync_word[1] = 0x34U;
    assert(lr11xx_configure(&dev, &config) == ESP_ERR_INVALID_ARG);
    assert(chip.count == 0U);
}

static void test_probe(void)
{
    const struct {
        uint8_t code;
        lr11xx_part_t part;
        const char *name;
    } cases[] = {
        {0x01U, LR11XX_PART_LR1110, "LR1110"},
        {0x02U, LR11XX_PART_LR1120, "LR1120"},
        {0x03U, LR11XX_PART_LR1121, "LR1121"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        lr11xx_t dev;
        reset_chip();
        chip.version[1] = cases[i].code;
        assert(open_device(&dev, 0) == ESP_OK);
        lr11xx_version_t version = {0};
        assert(lr11xx_probe(&dev, &version) == ESP_OK);
        assert(version.part == cases[i].part);
        assert(version.device_code == cases[i].code);
        assert(version.hardware == 0x22U);
        assert(version.firmware_major == 0x01U);
        assert(version.firmware_minor == 0x0AU);
        assert(dev.part == cases[i].part);
        assert(strcmp(lr11xx_part_name(version.part), cases[i].name) == 0);
        /* GetVersion is one command window followed by one response read. */
        assert(count_windows(LR11XX_OP_GET_VERSION) == 1U);
        assert(chip.reads == 1U);
    }

    /* A bus that reads back all zeroes or all ones has no part on it, and an
     * unfamiliar type byte is a family member this driver has not been taught,
     * so neither may be reported as a working radio. */
    const uint8_t absent[] = {0x00U, 0xFFU, 0x7AU};
    for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
        lr11xx_t dev;
        reset_chip();
        chip.version[1] = absent[i];
        assert(open_device(&dev, 0) == ESP_OK);
        assert(lr11xx_probe(&dev, NULL) == ESP_ERR_NOT_FOUND);
        assert(dev.part == LR11XX_PART_UNKNOWN);
    }

    /* A status byte saying the command failed or was not understood means
     * the bytes after it are not an answer. */
    const uint8_t rejected[] = {0x00U, 0x02U};
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        lr11xx_t dev;
        reset_chip();
        chip.stat1 = rejected[i];
        assert(open_device(&dev, 0) == ESP_OK);
        assert(lr11xx_probe(&dev, NULL) == ESP_ERR_INVALID_RESPONSE);
    }
}

static void test_configure_windows(void)
{
    lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 3300U) == ESP_OK);
    const solar_os_radio_config_t config = lora_config();
    assert(lr11xx_configure(&dev, &config) == ESP_OK);

    /* Every window the driver opened carried a known two-byte opcode from the
     * system or radio group, never a one-byte SX126x-style command. */
    assert(chip.count > 0);
    for (size_t i = 0; i < chip.count; i++) {
        const uint8_t group = (uint8_t)(chip.windows[i].opcode >> 8);
        assert(group == 0x01U || group == 0x02U);
    }

    const window_t *frequency = find_window(LR11XX_OP_SET_RF_FREQUENCY);
    assert(frequency != NULL);
    assert(frequency->param_len == 4U);
    assert(frequency->params[0] == 0x36U);
    assert(frequency->params[1] == 0x89U);
    assert(frequency->params[2] == 0xCAU);
    assert(frequency->params[3] == 0xC0U);

    const window_t *packet_type = find_window(LR11XX_OP_SET_PACKET_TYPE);
    assert(packet_type != NULL);
    assert(packet_type->param_len == 1U);
    assert(packet_type->params[0] == 0x02U); /* LoRa */

    /* One command and one byte, where the SX1262 needs two nibble-mapped
     * register writes. */
    const window_t *sync = find_window(LR11XX_OP_SET_LORA_SYNC_WORD);
    assert(sync != NULL);
    assert(sync->param_len == 1U);
    assert(sync->params[0] == 0x12U);

    /* The interrupt word is 32 bits here, so both the mask command and the
     * clear command are wider than their SX1262 counterparts. */
    const window_t *irq_params = find_window(LR11XX_OP_SET_DIO_IRQ_PARAMS);
    assert(irq_params != NULL);
    assert(irq_params->param_len == 8U);
    /* IRQ2 is left unarmed: these boards wire a single interrupt line. */
    assert(irq_params->params[4] == 0x00U);
    assert(irq_params->params[5] == 0x00U);
    assert(irq_params->params[6] == 0x00U);
    assert(irq_params->params[7] == 0x00U);

    const window_t *clear = find_window(LR11XX_OP_CLEAR_IRQ);
    assert(clear != NULL);
    assert(clear->param_len == 4U);

    const window_t *modulation = find_window(LR11XX_OP_SET_MODULATION_PARAMS);
    assert(modulation != NULL);
    assert(modulation->param_len == 4U);
    const window_t *packet = find_window(LR11XX_OP_SET_PACKET_PARAMS);
    assert(packet != NULL);
    assert(packet->param_len == 6U);

    const window_t *pa = find_window(LR11XX_OP_SET_PA_CONFIG);
    assert(pa != NULL);
    assert(pa->param_len == 4U);
    assert(pa->params[0] == 0x00U); /* 14 dBm stays on the low-power PA */
    const window_t *tx_params = find_window(LR11XX_OP_SET_TX_PARAMS);
    assert(tx_params != NULL);
    assert(tx_params->param_len == 2U);
    assert((int8_t)tx_params->params[0] == 14);

    /* Standby comes before anything is configured, and the image calibration
     * for the chosen band before the modulation is set. */
    assert(index_of(LR11XX_OP_SET_STANDBY) < index_of(LR11XX_OP_SET_REG_MODE));
    assert(index_of(LR11XX_OP_CALIBRATE_IMAGE) < index_of(LR11XX_OP_SET_MODULATION_PARAMS));
    assert(index_of(LR11XX_OP_SET_RF_FREQUENCY) < index_of(LR11XX_OP_CALIBRATE_IMAGE));

    assert(dev.state == SOLAR_OS_RADIO_STATE_STANDBY);
}

static void test_tcxo(void)
{
    lr11xx_t dev;

    /* A board with a TCXO gets the supply command, and the clock change is
     * followed by a recalibration - skipping that leaves the part running on
     * a calibration taken against the wrong reference. */
    reset_chip();
    assert(open_device(&dev, 3300U) == ESP_OK);
    const solar_os_radio_config_t config = lora_config();
    assert(lr11xx_configure(&dev, &config) == ESP_OK);
    const window_t *tcxo = find_window(LR11XX_OP_SET_TCXO_MODE);
    assert(tcxo != NULL);
    assert(tcxo->param_len == 4U);
    assert(tcxo->params[0] == 0x07U); /* 3.3 V */
    /* 5 ms is 163 ticks of 30.52 us. On the bench a thousand times that
     * held BUSY for five seconds and read as a radio that hangs after
     * identifying itself. */
    assert(tcxo->params[1] == 0x00U && tcxo->params[2] == 0x00U &&
           tcxo->params[3] == 163U);
    assert(index_of(LR11XX_OP_SET_TCXO_MODE) < index_of(LR11XX_OP_CALIBRATE));

    /* The 3.0 V boards take the neighbouring code. */
    reset_chip();
    assert(open_device(&dev, 3000U) == ESP_OK);
    assert(lr11xx_configure(&dev, &config) == ESP_OK);
    tcxo = find_window(LR11XX_OP_SET_TCXO_MODE);
    assert(tcxo != NULL && tcxo->params[0] == 0x06U);

    /* A crystal board must never see the command: telling a part to power a
     * TCXO that is not there stalls its clock start-up. */
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    assert(lr11xx_configure(&dev, &config) == ESP_OK);
    assert(find_window(LR11XX_OP_SET_TCXO_MODE) == NULL);
}

static void test_2g4_refused(void)
{
    /* The LR1120 and LR1121 can reach 2.4 GHz, but only through the
     * high-frequency amplifier and only if the board's antenna switch is
     * programmed. Neither is implemented, so asking for that band must fail
     * loudly instead of transmitting into the sub-GHz port. */
    lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    solar_os_radio_config_t config = lora_config();
    config.frequency_hz = 2450000000U;
    assert(lr11xx_configure(&dev, &config) == ESP_ERR_NOT_SUPPORTED);
    assert(chip.count == 0U);
}

static void test_send(void)
{
    lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    const solar_os_radio_config_t config = lora_config();
    assert(lr11xx_configure(&dev, &config) == ESP_OK);

    const size_t before = chip.count;
    chip.irq = IRQ_TX_DONE;
    solar_os_radio_packet_t packet = {0};
    const uint8_t payload[] = {0xDEU, 0xADU, 0xBEU, 0xEFU, 0x01U};
    packet.len = sizeof(payload);
    memcpy(packet.data, payload, sizeof(payload));
    assert(lr11xx_send(&dev, &packet, 100U) == ESP_OK);

    /* WriteBuffer8 carries the payload alone. The SX1262's WriteBuffer takes a
     * leading offset byte; sending one here would transmit it as data and
     * truncate the last byte. */
    const window_t *write = NULL;
    for (size_t i = before; i < chip.count; i++) {
        if (chip.windows[i].opcode == LR11XX_OP_WRITE_BUFFER8) {
            write = &chip.windows[i];
        }
    }
    assert(write != NULL);
    assert(write->param_len == sizeof(payload));
    assert(memcmp(write->params, payload, sizeof(payload)) == 0);

    /* The payload length is part of the packet parameters, so they are re-sent
     * for this length before the buffer is filled. */
    size_t packet_params = 0;
    size_t write_index = 0;
    size_t tx_index = 0;
    for (size_t i = before; i < chip.count; i++) {
        if (chip.windows[i].opcode == LR11XX_OP_SET_PACKET_PARAMS) {
            packet_params = i;
            assert(chip.windows[i].params[3] == sizeof(payload));
        } else if (chip.windows[i].opcode == LR11XX_OP_WRITE_BUFFER8) {
            write_index = i;
        } else if (chip.windows[i].opcode == LR11XX_OP_SET_TX) {
            tx_index = i;
        }
    }
    assert(packet_params < write_index);
    assert(write_index < tx_index);
    assert(dev.state == SOLAR_OS_RADIO_STATE_STANDBY);

    /* A transmit that never completes must time out rather than spin. */
    chip.irq = 0UL;
    assert(lr11xx_send(&dev, &packet, 20U) == ESP_ERR_TIMEOUT);

    /* Oversized and empty payloads are refused before any bus traffic. */
    solar_os_radio_packet_t bad = {0};
    bad.len = 0;
    assert(lr11xx_send(&dev, &bad, 10U) == ESP_ERR_INVALID_ARG);
    bad.len = LR11XX_MAX_PACKET_LEN + 1U;
    assert(lr11xx_send(&dev, &bad, 10U) == ESP_ERR_INVALID_ARG);
}

/* With an implicit header the receiver decodes the length it was configured
 * with, so a send is that length or nothing: it is refused before the part
 * is touched. A length left at zero is the longest the part carries under
 * either header, and is reported as such. */
static void test_implicit_length_send_must_match(void)
{
    lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    solar_os_radio_config_t config = lora_config();
    config.variable_length = false;
    config.payload_length = 20U;
    assert(lr11xx_configure(&dev, &config) == ESP_OK);

    solar_os_radio_packet_t packet = {0};
    packet.len = 5U;
    size_t before = chip.count;
    chip.irq = IRQ_TX_DONE;
    assert(lr11xx_send(&dev, &packet, 100U) == ESP_ERR_INVALID_SIZE);
    assert(chip.count == before);
    assert(dev.state == SOLAR_OS_RADIO_STATE_STANDBY);

    packet.len = 20U;
    before = chip.count;
    assert(lr11xx_send(&dev, &packet, 100U) == ESP_OK);
    const window_t *params = NULL;
    for (size_t i = before; i < chip.count; i++) {
        if (chip.windows[i].opcode == LR11XX_OP_SET_PACKET_PARAMS) {
            params = &chip.windows[i];
        }
    }
    assert(params != NULL);
    assert(params->params[2] == 0x01U); /* implicit header */
    assert(params->params[3] == 20U);

    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    config = lora_config();
    config.variable_length = false;
    config.payload_length = 0U;
    assert(lr11xx_configure(&dev, &config) == ESP_OK);
    solar_os_radio_status_t status;
    assert(lr11xx_get_status(&dev, &status) == ESP_OK);
    assert(status.config.payload_length == LR11XX_MAX_PACKET_LEN);
    params = find_window(LR11XX_OP_SET_PACKET_PARAMS);
    assert(params != NULL && params->params[3] == LR11XX_MAX_PACKET_LEN);
    packet.len = 5U;
    assert(lr11xx_send(&dev, &packet, 100U) == ESP_ERR_INVALID_SIZE);
    packet.len = LR11XX_MAX_PACKET_LEN;
    chip.irq = IRQ_TX_DONE;
    assert(lr11xx_send(&dev, &packet, 100U) == ESP_OK);

    /* Under an explicit header a length left at zero is the same longest
     * length, reported and given to the part, and any send fits in it. */
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    config = lora_config();
    config.variable_length = true;
    config.payload_length = 0U;
    assert(lr11xx_configure(&dev, &config) == ESP_OK);
    assert(lr11xx_get_status(&dev, &status) == ESP_OK);
    assert(status.config.payload_length == LR11XX_MAX_PACKET_LEN);
    params = find_window(LR11XX_OP_SET_PACKET_PARAMS);
    assert(params != NULL && params->params[2] == 0x00U); /* explicit header */
    assert(params->params[3] == LR11XX_MAX_PACKET_LEN);
    packet.len = 5U;
    chip.irq = IRQ_TX_DONE;
    assert(lr11xx_send(&dev, &packet, 100U) == ESP_OK);
    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_RX) == ESP_OK);
    assert(chip.max_len == LR11XX_MAX_PACKET_LEN);
}

/*
 * The packet length the part is given for a transmit is also the longest
 * packet it will accept while listening. After a short send, entering
 * receive has to put the maximum back, or the radio is deaf to anything
 * longer than the last thing it said.
 */
static void test_rx_after_short_send(void)
{
    /* Both ways into receive - asking for the state and simply polling for a
     * packet - put the maximum length back before SetRx. */
    for (int poll = 0; poll < 2; poll++) {
        lr11xx_t dev;
        reset_chip();
        assert(open_device(&dev, 0) == ESP_OK);
        const solar_os_radio_config_t config = lora_config();
        assert(lr11xx_configure(&dev, &config) == ESP_OK);
        chip.irq = IRQ_TX_DONE;
        solar_os_radio_packet_t packet = {0};
        packet.len = 5U;
        assert(lr11xx_send(&dev, &packet, 100U) == ESP_OK);

        const size_t before = chip.count;
        chip.irq = 0UL;
        if (poll) {
            assert(lr11xx_receive(&dev, &packet, 0U) == ESP_ERR_TIMEOUT);
        } else {
            assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_RX) == ESP_OK);
        }
        assert(dev.state == SOLAR_OS_RADIO_STATE_RX);
        size_t params = 0;
        size_t rx = 0;
        for (size_t i = before; i < chip.count; i++) {
            if (chip.windows[i].opcode == LR11XX_OP_SET_PACKET_PARAMS) {
                params = i;
                assert(chip.windows[i].params[3] == LR11XX_MAX_PACKET_LEN);
            } else if (chip.windows[i].opcode == LR11XX_OP_SET_RX) {
                rx = i;
            }
        }
        assert(params != 0 && rx != 0 && params < rx);
    }
}

static void test_receive(void)
{
    lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    const solar_os_radio_config_t config = lora_config();
    assert(lr11xx_configure(&dev, &config) == ESP_OK);

    const uint8_t frame[] = {0x10U, 0x20U, 0x30U, 0x40U};
    chip.rx_start = 3U;
    chip.rx_len = (uint8_t)sizeof(frame);
    memcpy(&chip.rx_payload[chip.rx_start], frame, sizeof(frame));
    chip.rssi_raw = 180U; /* -90 dBm */
    chip.snr_raw = 20;    /* +5 dB */
    chip.irq = IRQ_RX_DONE;

    size_t before = chip.count;
    solar_os_radio_packet_t packet = {0};
    assert(lr11xx_receive(&dev, &packet, 100U) == ESP_OK);
    assert(packet.len == sizeof(frame));
    assert(memcmp(packet.data, frame, sizeof(frame)) == 0);
    assert(packet.crc_ok);
    assert(packet.has_rssi && packet.rssi_dbm == -90);
    assert(packet.has_snr && packet.snr_db == 5);

    /* ReadBuffer8 does take an offset, and it is the start pointer the chip
     * reported rather than a fixed zero. */
    const window_t *read = NULL;
    for (size_t i = before; i < chip.count; i++) {
        if (chip.windows[i].opcode == LR11XX_OP_READ_BUFFER8) {
            read = &chip.windows[i];
        }
    }
    assert(read != NULL);
    assert(read->param_len == 2U);
    assert(read->params[0] == 3U);
    assert(read->params[1] == (uint8_t)sizeof(frame));
    assert(dev.state == SOLAR_OS_RADIO_STATE_RX);

    /* Receive is a poll, not a session: a second call must not re-arm the
     * receiver, or the part spends its time restarting instead of listening. */
    before = chip.count;
    chip.irq = IRQ_RX_DONE;
    solar_os_radio_packet_t again = {0};
    assert(lr11xx_receive(&dev, &again, 100U) == ESP_OK);
    for (size_t i = before; i < chip.count; i++) {
        assert(chip.windows[i].opcode != LR11XX_OP_SET_RX);
    }

    /* Nothing received is a timeout, and the part is left listening. */
    chip.irq = 0UL;
    solar_os_radio_packet_t empty = {0};
    assert(lr11xx_receive(&dev, &empty, 20U) == ESP_ERR_TIMEOUT);
    assert(dev.state == SOLAR_OS_RADIO_STATE_RX);

    /* The cached metrics travel with the status. */
    solar_os_radio_status_t status = {0};
    assert(lr11xx_get_status(&dev, &status) == ESP_OK);
    assert(status.has_rssi && status.rssi_dbm == -90);
    assert(status.has_snr && status.snr_db == 5);
    assert(status.config.frequency_hz == 915000000U);
}

static void test_pa_table(void)
{
    /* The amplifier setup is measured board tuning, not a formula: the value
     * handed to SetTxParams is not the requested power, and the duty cycle
     * moves non-monotonically. A single "safe" operating point would get the
     * radiated power wrong at nearly every level. */
    const struct {
        int8_t request;
        uint8_t pa_sel;
        uint8_t supply;
        uint8_t duty;
        uint8_t hp_sel;
        int8_t tx_power;
    } cases[] = {
        {-17, 0x00U, 0x00U, 0x00U, 0x00U, -15},
        {0, 0x00U, 0x00U, 0x00U, 0x00U, 3},
        {10, 0x00U, 0x00U, 0x01U, 0x00U, 14},
        {14, 0x00U, 0x00U, 0x05U, 0x00U, 14},
        /* 16 dBm is where the reference board crosses to the high-power
         * amplifier, and that path runs off VBAT rather than the regulator. */
        {16, 0x01U, 0x01U, 0x03U, 0x03U, 22},
        {20, 0x01U, 0x01U, 0x03U, 0x07U, 22},
        {22, 0x01U, 0x01U, 0x04U, 0x07U, 22},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        lr11xx_t dev;
        reset_chip();
        assert(open_device(&dev, 0) == ESP_OK);
        solar_os_radio_config_t config = lora_config();
        config.tx_power_dbm = cases[i].request;
        assert(lr11xx_configure(&dev, &config) == ESP_OK);

        const window_t *pa = find_window(LR11XX_OP_SET_PA_CONFIG);
        assert(pa != NULL);
        assert(pa->params[0] == cases[i].pa_sel);
        assert(pa->params[1] == cases[i].supply);
        assert(pa->params[2] == cases[i].duty);
        assert(pa->params[3] == cases[i].hp_sel);

        const window_t *tx = find_window(LR11XX_OP_SET_TX_PARAMS);
        assert(tx != NULL);
        assert((int8_t)tx->params[0] == cases[i].tx_power);
        assert(tx->params[1] == 0x04U); /* 80 us ramp on this family */
        assert(index_of(LR11XX_OP_SET_PA_CONFIG) < index_of(LR11XX_OP_SET_TX_PARAMS));
    }

    /* Requests off either end of the table are refused before anything is
     * sent, rather than indexing off it or transmitting at another power. */
    lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    solar_os_radio_config_t config = lora_config();
    config.tx_power_dbm = 100;
    assert(lr11xx_configure(&dev, &config) == ESP_ERR_INVALID_ARG);
    config.tx_power_dbm = -100;
    assert(lr11xx_configure(&dev, &config) == ESP_ERR_INVALID_ARG);
    assert(chip.count == 0U);
}

static void test_snr_rounding(void)
{
    /* SNR is not the SX126x's plain raw/4. The byte is signed and the
     * conversion adds 2 before dividing, so it rounds to nearest. Truncating
     * instead quietly loses a decibel of reported link margin. */
    const struct { int8_t raw; int16_t db; } cases[] = {
        {20, 5},  /* the same either way */
        {10, 3},  /* (10 + 2) >> 2 is 3; a plain 10/4 would say 2 */
        {9, 2},
        {-20, -5},
        {0, 0},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        lr11xx_t dev;
        reset_chip();
        assert(open_device(&dev, 0) == ESP_OK);
        const solar_os_radio_config_t config = lora_config();
        assert(lr11xx_configure(&dev, &config) == ESP_OK);

        chip.rx_start = 0U;
        chip.rx_len = 1U;
        chip.rx_payload[0] = 0x99U;
        chip.rssi_raw = 200U; /* -100 dBm */
        chip.snr_raw = cases[i].raw;
        chip.irq = IRQ_RX_DONE;

        solar_os_radio_packet_t packet = {0};
        assert(lr11xx_receive(&dev, &packet, 100U) == ESP_OK);
        assert(packet.has_snr);
        assert(packet.snr_db == cases[i].db);
        assert(packet.rssi_dbm == -100);
    }
}

static void test_sleep_and_wake(void)
{
    lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    const solar_os_radio_config_t config = lora_config();
    assert(lr11xx_configure(&dev, &config) == ESP_OK);

    const unsigned wakes = chip.wakes;
    size_t before = chip.count;
    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_SLEEP) == ESP_OK);
    assert(dev.state == SOLAR_OS_RADIO_STATE_SLEEP);
    assert(chip.wakes == wakes);

    /* Sleep must retain the configuration. A cold sleep loses all of it and
     * nothing here reconfigures on wake, so the radio would come back
     * silently unconfigured. */
    const window_t *sleep = NULL;
    for (size_t i = before; i < chip.count; i++) {
        if (chip.windows[i].opcode == LR11XX_OP_SET_SLEEP) {
            sleep = &chip.windows[i];
        }
    }
    assert(sleep != NULL);
    assert(sleep->param_len == 5U);
    assert((sleep->params[0] & 0x01U) != 0U); /* warm start */
    assert((sleep->params[0] & 0x02U) == 0U); /* no RTC wake-up */

    /* A sleeping part takes no command, so asking again sends none. */
    before = chip.count;
    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_SLEEP) == ESP_OK);
    assert(chip.count == before);

    /* Leaving sleep needs chip select to fall first; the part cannot see a
     * command until it has woken. The documented wake holds chip select low
     * for at least 100 us, which at the driver's 2 MHz is 25 bytes of NOP. */
    before = chip.count;
    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_STANDBY) == ESP_OK);
    assert(chip.wakes == wakes + 1U);
    assert(chip.wake_bytes >= 25U);
    assert(dev.state == SOLAR_OS_RADIO_STATE_STANDBY);

    /* Waking from a retention sleep corrupts one internal parameter and
     * spoils the adjacent-channel power of every later LoRa transmission.
     * The published fix is to clear bit 30 of 0x00F30054 on the way out. */
    const window_t *fix = NULL;
    for (size_t i = before; i < chip.count; i++) {
        if (chip.windows[i].opcode == LR11XX_OP_WRITE_REG_MEM32_MASK) {
            fix = &chip.windows[i];
        }
    }
    assert(fix != NULL);
    assert(fix->param_len == 12U);
    assert(fix->params[0] == 0x00U && fix->params[1] == 0xF3U &&
           fix->params[2] == 0x00U && fix->params[3] == 0x54U);
    assert(fix->params[4] == 0x40U && fix->params[5] == 0x00U &&
           fix->params[6] == 0x00U && fix->params[7] == 0x00U);
    assert(fix->params[8] == 0x00U && fix->params[9] == 0x00U &&
           fix->params[10] == 0x00U && fix->params[11] == 0x00U);
    assert(index_of(LR11XX_OP_WRITE_REG_MEM32_MASK) < chip.count);
}

static void test_busy_timeout(void)
{
    lr11xx_t dev;
    reset_chip();
    busy_level = 1;
    spins = 0U;
    sleeps = 0U;
    assert(open_device(&dev, 0) == ESP_ERR_INVALID_RESPONSE);
    assert(chip.count == 0U);
    /* A part that never drops BUSY is waited for asleep, not in a spin that
     * holds the processor for the whole second. */
    assert(spins <= 4U);
    assert(sleeps > 100U);
}

static void test_null_arguments(void)
{
    lr11xx_t dev;
    reset_chip();
    assert(lr11xx_init(NULL, "spi0", 39, 41, 45, 42, 2000000U, 0, NULL) == ESP_ERR_INVALID_ARG);
    assert(lr11xx_init(&dev, NULL, 39, 41, 45, 42, 2000000U, 0, NULL) == ESP_ERR_INVALID_ARG);
    assert(open_device(&dev, 0) == ESP_OK);
    assert(lr11xx_configure(&dev, NULL) == ESP_ERR_INVALID_ARG);
    assert(lr11xx_probe(NULL, NULL) == ESP_ERR_INVALID_ARG);
    assert(lr11xx_get_status(&dev, NULL) == ESP_ERR_INVALID_ARG);
    assert(lr11xx_send(&dev, NULL, 10U) == ESP_ERR_INVALID_ARG);
    {
        /* A send given no time is refused before the transmitter is keyed. */
        const solar_os_radio_config_t config = lora_config();
        assert(lr11xx_configure(&dev, &config) == ESP_OK);
        solar_os_radio_packet_t packet = {0};
        packet.len = 4U;
        const size_t before = chip.count;
        assert(lr11xx_send(&dev, &packet, 0U) == ESP_ERR_INVALID_ARG);
        assert(chip.count == before);
    }
    assert(lr11xx_receive(&dev, NULL, 10U) == ESP_ERR_INVALID_ARG);
    assert(lr11xx_send_stream(&dev, NULL, 4U, 10U) == ESP_ERR_INVALID_ARG);
    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_UNKNOWN) == ESP_ERR_INVALID_ARG);
}

static void test_status_read(void)
{
    /* The LR11xx answers any read with stat1, stat2 and the interrupt word,
     * so there is no GetIrqStatus opcode. Sending one - 0x0100, GetStatus -
     * and then reading would shift the interrupt word by a byte and make
     * TxDone look like nothing at all. */
    lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    const solar_os_radio_config_t config = lora_config();
    assert(lr11xx_configure(&dev, &config) == ESP_OK);

    chip.irq = IRQ_TX_DONE;
    solar_os_radio_packet_t packet = {0};
    packet.len = 2U;
    packet.data[0] = 0xA5U;
    packet.data[1] = 0x5AU;
    assert(lr11xx_send(&dev, &packet, 100U) == ESP_OK);

    assert(chip.status_reads > 0U);
    assert(find_window(0x0100U) == NULL);
}

static void test_rf_switch(void)
{
    lr11xx_t dev;
    const solar_os_radio_config_t config = lora_config();

    /* A board that states a table gets it relayed verbatim, in standby, before
     * the radio is put on a frequency. */
    reset_chip();
    assert(open_device_with_switch(&dev) == ESP_OK);
    assert(lr11xx_configure(&dev, &config) == ESP_OK);
    const window_t *rfsw = find_window(LR11XX_OP_SET_DIO_AS_RF_SWITCH);
    assert(rfsw != NULL);
    assert(rfsw->param_len == 8U);
    assert(rfsw->params[0] == 0x03U); /* enable DIO5 and DIO6 */
    assert(rfsw->params[1] == 0x00U); /* standby */
    assert(rfsw->params[2] == 0x01U); /* rx: DIO5 */
    assert(rfsw->params[3] == 0x03U); /* tx: both */
    assert(rfsw->params[4] == 0x02U); /* tx_hp: DIO6 */
    assert(rfsw->params[5] == 0x00U);
    assert(rfsw->params[6] == 0x00U);
    assert(rfsw->params[7] == 0x00U);
    assert(index_of(LR11XX_OP_SET_STANDBY) < index_of(LR11XX_OP_SET_DIO_AS_RF_SWITCH));
    assert(index_of(LR11XX_OP_SET_DIO_AS_RF_SWITCH) < index_of(LR11XX_OP_SET_RF_FREQUENCY));

    /* A board that states nothing must not have a table invented for it: the
     * wrong table is worse than none, because it drives the switch to the
     * wrong port instead of leaving the DIOs in high impedance. */
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    assert(lr11xx_configure(&dev, &config) == ESP_OK);
    assert(find_window(LR11XX_OP_SET_DIO_AS_RF_SWITCH) == NULL);
}

/* GetErrors is two big-endian bytes; a clean configure reads back zero and
 * a failed calibration sets a bit. */
static void test_get_errors(void)
{
    lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 3300U) == ESP_OK);
    uint16_t errors = 0xFFFFU;
    assert(lr11xx_get_errors(&dev, &errors) == ESP_OK);
    assert(errors == 0U);
    chip.errors[0] = 0x00U;
    chip.errors[1] = 0x20U; /* HF crystal start */
    assert(lr11xx_get_errors(&dev, &errors) == ESP_OK);
    assert(errors == 0x0020U);
    assert(count_windows(LR11XX_OP_GET_ERRORS) == 2U);
}

/* A query may come while the part is asleep, when it would not answer. It
 * is woken first and left in standby; one that will not wake is lost track
 * of, like any other that stops answering. */
static void test_a_query_wakes_a_sleeping_part(void)
{
    lr11xx_t dev;
    uint16_t errors = 0xFFFFU;
    lr11xx_version_t version;

    reset_chip();
    assert(open_device(&dev, 3300U) == ESP_OK);
    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_SLEEP) == ESP_OK);
    unsigned wakes = chip.wakes;
    assert(lr11xx_get_errors(&dev, &errors) == ESP_OK);
    assert(chip.wakes == wakes + 1U);
    assert(dev.state == SOLAR_OS_RADIO_STATE_STANDBY);

    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_SLEEP) == ESP_OK);
    wakes = chip.wakes;
    assert(lr11xx_probe(&dev, &version) == ESP_OK);
    assert(chip.wakes == wakes + 1U);
    assert(dev.state == SOLAR_OS_RADIO_STATE_STANDBY);

    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_SLEEP) == ESP_OK);
    chip.fail_transfer = 1U; /* the wake is lost */
    assert(lr11xx_get_errors(&dev, &errors) != ESP_OK);
    assert(dev.state == SOLAR_OS_RADIO_STATE_UNKNOWN);
}

/* Status while listening with nothing heard yet reads the channel, so a
 * quiet band shows its noise floor rather than nothing at all. Once a
 * packet has arrived its own level and SNR are what status reports. */
static void test_status_rssi(void)
{
    lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    solar_os_radio_config_t config = lora_config();
    assert(lr11xx_configure(&dev, &config) == ESP_OK);
    solar_os_radio_status_t status;
    assert(lr11xx_get_status(&dev, &status) == ESP_OK);
    assert(!status.has_rssi);
    assert(count_windows(LR11XX_OP_GET_RSSI_INST) == 0U);

    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_RX) == ESP_OK);
    chip.rssi_inst_raw = 210U; /* -105 dBm */
    assert(lr11xx_get_status(&dev, &status) == ESP_OK);
    assert(status.has_rssi);
    assert(status.rssi_dbm == -105);
    assert(!status.has_snr);
    assert(count_windows(LR11XX_OP_GET_RSSI_INST) == 1U);
}

/* Low-data-rate optimize follows the symbol time, at every bandwidth. Both
 * ends of a link have to agree on it, so a rule that covers only the narrow
 * bandwidths cannot talk to a radio that follows the modulation's own. */
static void test_low_data_rate_follows_symbol_time(void)
{
    const struct { uint8_t sf; uint32_t bw_hz; uint8_t ldro; } cases[] = {
        {10U, 125000U, 0U}, /*  8.2 ms */
        {11U, 125000U, 1U}, /* 16.4 ms */
        {12U, 125000U, 1U}, /* 32.8 ms */
        {11U, 250000U, 0U}, /*  8.2 ms */
        {12U, 250000U, 1U}, /* 16.4 ms */
        {12U, 500000U, 0U}, /*  8.2 ms */
        {7U, 10400U, 0U},   /* 12.3 ms */
        {8U, 10400U, 1U},   /* 24.6 ms */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        solar_os_radio_config_t config = lora_config();
        config.spreading_factor = cases[i].sf;
        config.rx_bandwidth_hz = cases[i].bw_hz;
        const window_t *window = configure_and_find(&config, LR11XX_OP_SET_MODULATION_PARAMS);
        assert(window->params[0] == cases[i].sf);
        assert(window->params[3] == cases[i].ldro);
    }
}

/* The bandwidth reported is the one the part is on, which is the next one
 * up it offers from the one asked for. */
static void test_bandwidth_reported_as_applied(void)
{
    lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    solar_os_radio_config_t config = lora_config();
    config.rx_bandwidth_hz = 100000U;
    assert(lr11xx_configure(&dev, &config) == ESP_OK);
    solar_os_radio_status_t status;
    assert(lr11xx_get_status(&dev, &status) == ESP_OK);
    assert(status.config.rx_bandwidth_hz == 125000U);
}

/* Listening asks for the longest preamble there is, so a sender with a long
 * one is heard; sending uses the configured length. */
static void test_listening_takes_any_preamble(void)
{
    lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    solar_os_radio_config_t config = lora_config();
    config.preamble_len = 8U;
    assert(lr11xx_configure(&dev, &config) == ESP_OK);

    size_t before = chip.count;
    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_RX) == ESP_OK);
    const window_t *params = NULL;
    for (size_t i = before; i < chip.count; i++) {
        if (chip.windows[i].opcode == LR11XX_OP_SET_PACKET_PARAMS) {
            params = &chip.windows[i];
        }
    }
    assert(params != NULL && params->params[0] == 0xFFU && params->params[1] == 0xFFU);

    before = chip.count;
    chip.irq = IRQ_TX_DONE;
    solar_os_radio_packet_t packet = {0};
    packet.len = 3U;
    assert(lr11xx_send(&dev, &packet, 100U) == ESP_OK);
    params = NULL;
    for (size_t i = before; i < chip.count; i++) {
        if (chip.windows[i].opcode == LR11XX_OP_SET_PACKET_PARAMS) {
            params = &chip.windows[i];
        }
    }
    assert(params != NULL && params->params[0] == 0x00U && params->params[1] == 8U);

    before = chip.count;
    chip.irq = 0UL;
    assert(lr11xx_receive(&dev, &packet, 0U) == ESP_ERR_TIMEOUT);
    params = NULL;
    for (size_t i = before; i < chip.count; i++) {
        if (chip.windows[i].opcode == LR11XX_OP_SET_PACKET_PARAMS) {
            params = &chip.windows[i];
        }
    }
    assert(params != NULL && params->params[0] == 0xFFU && params->params[1] == 0xFFU);
}

/* The lock is made by lr11xx_init(), so a device that never had one is
 * refused rather than given a lock by whoever calls first. */
static void test_uninitialised_device_is_refused(void)
{
    lr11xx_t dev;
    memset(&dev, 0, sizeof(dev));
    reset_chip();
    const solar_os_radio_config_t config = lora_config();
    solar_os_radio_status_t status;
    assert(lr11xx_configure(&dev, &config) == ESP_ERR_INVALID_STATE);
    assert(lr11xx_get_status(&dev, &status) == ESP_ERR_INVALID_STATE);
    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_STANDBY) == ESP_ERR_INVALID_STATE);
    assert(chip.count == 0U);
    assert(open_device(&dev, 0) == ESP_OK);
    assert(dev.mutex != NULL);
}

/* --- faults -------------------------------------------------------------- */

/* Nothing is sent or heard on settings nobody gave. */
static void test_unconfigured_radio_is_refused(void)
{
    lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    const size_t before = chip.count;

    solar_os_radio_packet_t packet = {0};
    packet.len = 1U;
    chip.irq = IRQ_TX_DONE;
    assert(lr11xx_send(&dev, &packet, 50U) != ESP_OK);
    chip.irq = 0UL;
    assert(lr11xx_receive(&dev, &packet, 0U) != ESP_OK ||
           chip.mode != SOLAR_OS_RADIO_STATE_RX);
    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_RX) != ESP_OK);
    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_TX) != ESP_OK);
    for (size_t i = before; i < chip.count; i++) {
        assert(chip.windows[i].opcode != LR11XX_OP_SET_TX);
        assert(chip.windows[i].opcode != LR11XX_OP_SET_RX);
    }
}

/* Settings the part cannot take are refused, or reported as applied. */
static void test_settings_reported_as_applied(void)
{
    lr11xx_t dev;
    solar_os_radio_status_t status;

    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    solar_os_radio_config_t config = lora_config();
    config.tx_power_dbm = 30;
    if (lr11xx_configure(&dev, &config) == ESP_OK) {
        assert(lr11xx_get_status(&dev, &status) == ESP_OK);
        assert(status.config.tx_power_dbm <= 22);
    }

    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    config = lora_config();
    config.spreading_factor = 13U;
    if (lr11xx_configure(&dev, &config) == ESP_OK) {
        assert(lr11xx_get_status(&dev, &status) == ESP_OK);
        const window_t *modulation = find_window(LR11XX_OP_SET_MODULATION_PARAMS);
        assert(modulation != NULL);
        assert(status.config.spreading_factor == modulation->params[0]);
    }

    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    config = lora_config();
    config.coding_rate_denominator = 9U;
    if (lr11xx_configure(&dev, &config) == ESP_OK) {
        assert(lr11xx_get_status(&dev, &status) == ESP_OK);
        const window_t *modulation = find_window(LR11XX_OP_SET_MODULATION_PARAMS);
        assert(modulation != NULL);
        assert(status.config.coding_rate_denominator == modulation->params[2] + 4U);
    }
}

/* What the driver says of the radio has to be what the radio was last told:
 * listening means listening, and for the longest packet. A driver that no
 * longer knows may say unknown; it may not say something else. */
static void assert_state_true(const lr11xx_t *dev)
{
    switch (dev->state) {
    case SOLAR_OS_RADIO_STATE_RX:
        assert(chip.mode == SOLAR_OS_RADIO_STATE_RX);
        assert(chip.max_len == LR11XX_MAX_PACKET_LEN);
        break;
    case SOLAR_OS_RADIO_STATE_STANDBY:
    case SOLAR_OS_RADIO_STATE_SLEEP:
    case SOLAR_OS_RADIO_STATE_TX:
        assert(chip.mode == dev->state);
        break;
    case SOLAR_OS_RADIO_STATE_UNKNOWN:
    default:
        break;
    }
}

static void open_listening(lr11xx_t *dev)
{
    reset_chip();
    assert(open_device(dev, 0) == ESP_OK);
    const solar_os_radio_config_t config = lora_config();
    assert(lr11xx_configure(dev, &config) == ESP_OK);
    assert(lr11xx_set_state(dev, SOLAR_OS_RADIO_STATE_RX) == ESP_OK);
    assert_state_true(dev);
}


/* A packet's signal readings belong to the settings it was heard under. */
static void test_signal_readings_do_not_outlive_their_settings(void)
{
    lr11xx_t dev;
    open_listening(&dev);
    chip.irq = IRQ_RX_DONE;
    chip.rx_len = 4U;
    chip.rssi_raw = 180U;
    solar_os_radio_packet_t heard;
    assert(lr11xx_receive(&dev, &heard, 50U) == ESP_OK);
    solar_os_radio_status_t status;
    assert(lr11xx_get_status(&dev, &status) == ESP_OK);
    assert(status.has_rssi);

    chip.irq = 0UL;
    solar_os_radio_config_t other = lora_config();
    other.frequency_hz = 868000000U;
    assert(lr11xx_configure(&dev, &other) == ESP_OK);
    assert(lr11xx_get_status(&dev, &status) == ESP_OK);
    assert(!status.has_rssi && !status.has_snr);
}

/* An absent part leaves the bus reading all ones or all zeros. All ones is
 * neither a transmission that finished nor a packet that arrived, and all
 * zeros is not nothing heard: it is the part not answering, every time,
 * and a part that is not answering is not known to be listening. */
static void test_dead_bus(void)
{
    static const uint8_t levels[] = {0xFFU, 0x00U};
    for (size_t i = 0; i < sizeof(levels); i++) {
        lr11xx_t dev;
        solar_os_radio_packet_t heard;
        open_listening(&dev);
        chip.dead = true;
        chip.dead_byte = levels[i];
        assert(lr11xx_receive(&dev, &heard, 50U) == ESP_ERR_INVALID_RESPONSE);
        assert(heard.len == 0U);
        assert(dev.state == SOLAR_OS_RADIO_STATE_UNKNOWN);

        open_listening(&dev);
        chip.dead = true;
        chip.dead_byte = levels[i];
        solar_os_radio_packet_t packet = {0};
        packet.len = 5U;
        memcpy(packet.data, "hello", 5U);
        assert(lr11xx_send(&dev, &packet, 50U) == ESP_ERR_INVALID_RESPONSE);
        assert(dev.state == SOLAR_OS_RADIO_STATE_UNKNOWN);

        open_listening(&dev);
        chip.dead = true;
        chip.dead_byte = levels[i];
        lr11xx_version_t version;
        assert(lr11xx_probe(&dev, &version) == ESP_ERR_INVALID_RESPONSE);
    }
}

/* A command the part refuses is a configure that failed. */
static void test_rejected_command(void)
{
    lr11xx_t dev;
    reset_chip();
    assert(open_device(&dev, 0) == ESP_OK);
    const solar_os_radio_config_t config = lora_config();
    chip.reject_opcode = LR11XX_OP_SET_MODULATION_PARAMS;
    assert(lr11xx_configure(&dev, &config) != ESP_OK);
    assert(dev.state != SOLAR_OS_RADIO_STATE_STANDBY ||
           chip.mode == SOLAR_OS_RADIO_STATE_STANDBY);
}

/* A configure that fails at any transfer leaves the reported state true. */
static void test_configure_fault_at_every_step(void)
{
    lr11xx_t dev;
    open_listening(&dev);
    solar_os_radio_config_t other = lora_config();
    other.frequency_hz = 868000000U;
    const unsigned before = chip.transfers;
    assert(lr11xx_configure(&dev, &other) == ESP_OK);
    const unsigned steps = chip.transfers - before;
    assert(steps > 5U);

    for (unsigned step = 1U; step <= steps; step++) {
        open_listening(&dev);
        chip.fail_transfer = step;
        assert(lr11xx_configure(&dev, &other) != ESP_OK);
        assert_state_true(&dev);
        /* The settings reported are the ones in force, not the ones asked
         * for and not applied. */
        solar_os_radio_status_t status;
        assert(lr11xx_get_status(&dev, &status) == ESP_OK);
        assert(status.state == SOLAR_OS_RADIO_STATE_UNKNOWN ||
               status.config.frequency_hz != other.frequency_hz);
    }
}

/* After a failure the next configure brings the part back by hand, awake
 * and in standby, before it trusts it with settings again. */
static void test_configure_recovers_a_lost_part(void)
{
    lr11xx_t dev;
    open_listening(&dev);
    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_SLEEP) == ESP_OK);
    const solar_os_radio_config_t config = lora_config();
    /* The wake itself is lost, so the part is still asleep. */
    chip.fail_transfer = 1U;
    assert(lr11xx_configure(&dev, &config) != ESP_OK);
    assert(dev.state == SOLAR_OS_RADIO_STATE_UNKNOWN);
    assert(chip.mode == SOLAR_OS_RADIO_STATE_SLEEP);

    solar_os_radio_packet_t packet = {0};
    packet.len = 1U;
    assert(lr11xx_send(&dev, &packet, 50U) == ESP_ERR_INVALID_STATE);
    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_RX) == ESP_ERR_INVALID_STATE);

    const unsigned wakes = chip.wakes;
    assert(lr11xx_configure(&dev, &config) == ESP_OK);
    assert(chip.wakes == wakes + 1U);
    assert_state_true(&dev);
    assert(lr11xx_set_state(&dev, SOLAR_OS_RADIO_STATE_RX) == ESP_OK);
    assert_state_true(&dev);
}

/* A send that fails at any transfer fails, the clear after TX_DONE among
 * them, and the part is no longer vouched for: the standby that ends every
 * send still goes out, so a transmit in flight is stopped, and the state
 * reads unknown. */
static void test_send_fault_at_every_step(void)
{
    lr11xx_t dev;
    solar_os_radio_packet_t packet = {0};
    packet.len = 5U;
    memcpy(packet.data, "hello", 5U);

    open_listening(&dev);
    chip.irq = IRQ_TX_DONE;
    const unsigned before = chip.transfers;
    assert(lr11xx_send(&dev, &packet, 100U) == ESP_OK);
    const unsigned steps = chip.transfers - before;
    assert_state_true(&dev);

    for (unsigned step = 1U; step <= steps; step++) {
        open_listening(&dev);
        chip.irq = IRQ_TX_DONE;
        const size_t standbys_before = count_windows(LR11XX_OP_SET_STANDBY);
        chip.fail_transfer = step;
        assert(lr11xx_send(&dev, &packet, 100U) != ESP_OK);
        assert(dev.state == SOLAR_OS_RADIO_STATE_UNKNOWN);
        assert_state_true(&dev);
        /* The standby is sent whatever failed before it. The one transfer
         * it cannot be sent past is its own first one. */
        if (step != steps - 1U) {
            assert(count_windows(LR11XX_OP_SET_STANDBY) == standbys_before + 1U);
        }
    }
}

/* A packet whose RX_DONE could not be cleared is not delivered: the flag
 * would still be up at the next poll, and the same packet would be handed
 * back a second time. A receive that fails at any transfer, before the flag
 * is cleared or after, leaves the driver no longer knowing the part, and
 * saying so. */
static void test_receive_fault_at_every_step(void)
{
    lr11xx_t dev;
    solar_os_radio_packet_t heard;

    open_listening(&dev);
    chip.irq = IRQ_RX_DONE;
    chip.rx_len = 4U;
    chip.reject_opcode = LR11XX_OP_CLEAR_IRQ;
    assert(lr11xx_receive(&dev, &heard, 50U) == ESP_ERR_INVALID_RESPONSE);
    assert(heard.len == 0U);
    assert(dev.state == SOLAR_OS_RADIO_STATE_UNKNOWN);
    assert(lr11xx_receive(&dev, &heard, 50U) == ESP_ERR_INVALID_STATE);

    open_listening(&dev);
    chip.irq = IRQ_RX_DONE;
    chip.rx_len = 4U;
    const unsigned before = chip.transfers;
    assert(lr11xx_receive(&dev, &heard, 50U) == ESP_OK);
    const unsigned steps = chip.transfers - before;
    assert(steps > 3U);

    for (unsigned step = 1U; step <= steps; step++) {
        open_listening(&dev);
        const size_t clears_before = count_windows(LR11XX_OP_CLEAR_IRQ);
        chip.irq = IRQ_RX_DONE;
        chip.rx_len = 4U;
        chip.fail_transfer = step;
        const esp_err_t err = lr11xx_receive(&dev, &heard, 50U);
        assert_state_true(&dev);
        if (err == ESP_OK) {
            /* Delivered only once the flag is down: the receive's own clear
             * went through, and before the buffer was read. Only the signal
             * readings may fail without failing the receive. */
            assert(count_windows(LR11XX_OP_CLEAR_IRQ) == clears_before + 1U);
            assert(last_index_of(LR11XX_OP_CLEAR_IRQ) < last_index_of(LR11XX_OP_READ_BUFFER8));
            assert(heard.len == 4U);
        } else {
            /* Whichever transfer failed, before the flag was cleared or
             * after it, the part is no longer known to be listening. */
            assert(dev.state == SOLAR_OS_RADIO_STATE_UNKNOWN);
            assert(heard.len == 0U);
        }
    }
}

/* A part that stops answering while it is listening is not reported as
 * listening, and that is not the same answer as nothing heard. */
static void test_receive_notices_a_part_that_stops_answering(void)
{
    lr11xx_t dev;
    solar_os_radio_packet_t heard;
    solar_os_radio_status_t status;

    open_listening(&dev);
    assert(lr11xx_receive(&dev, &heard, 20U) == ESP_ERR_TIMEOUT);
    assert(dev.state == SOLAR_OS_RADIO_STATE_RX);

    busy_level = 1; /* as a part held in reset */
    assert(lr11xx_receive(&dev, &heard, 20U) == ESP_ERR_INVALID_RESPONSE);
    assert(dev.state == SOLAR_OS_RADIO_STATE_UNKNOWN);
    /* Without settings now, which is the caller's to mend, not the part's. */
    assert(lr11xx_receive(&dev, &heard, 20U) == ESP_ERR_INVALID_STATE);

    open_listening(&dev);
    busy_level = 1;
    assert(lr11xx_get_status(&dev, &status) == ESP_OK);
    assert(status.state == SOLAR_OS_RADIO_STATE_UNKNOWN && !status.has_rssi);

    /* Back, it takes a configure and is then itself again. */
    busy_level = 0;
    const solar_os_radio_config_t config = lora_config();
    assert(lr11xx_configure(&dev, &config) == ESP_OK);
    assert(lr11xx_receive(&dev, &heard, 20U) == ESP_ERR_TIMEOUT);
    assert_state_true(&dev);
}

int main(void)
{
    test_get_errors();
    test_a_query_wakes_a_sleeping_part();
    test_status_rssi();
    test_lora_encoding();
    test_config_limits();
    test_probe();
    test_configure_windows();
    test_tcxo();
    test_rf_switch();
    test_status_read();
    test_2g4_refused();
    test_send();
    test_implicit_length_send_must_match();
    test_rx_after_short_send();
    test_receive();
    test_pa_table();
    test_snr_rounding();
    test_sleep_and_wake();
    test_busy_timeout();
    test_null_arguments();
    test_unconfigured_radio_is_refused();
    test_settings_reported_as_applied();
    test_signal_readings_do_not_outlive_their_settings();
    test_dead_bus();
    test_rejected_command();
    test_configure_fault_at_every_step();
    test_configure_recovers_a_lost_part();
    test_send_fault_at_every_step();
    test_low_data_rate_follows_symbol_time();
    test_bandwidth_reported_as_applied();
    test_listening_takes_any_preamble();
    test_uninitialised_device_is_refused();
    test_receive_fault_at_every_step();
    test_receive_notices_a_part_that_stops_answering();
    puts("lr11xx tests: ok");
    return 0;
}
