#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "st25r3916.h"

#define SPACE_B_ACCESS 0xFBU
#define FIFO_WRITE 0x80U
#define FIFO_READ 0x9FU
#define READ_REGISTER 0x40U
#define CMD_TRANSMIT_WITH_CRC 0xC4U
#define CMD_TRANSMIT_WITHOUT_CRC 0xC5U
#define CMD_TRANSMIT_REQA 0xC6U

typedef struct {
    uint8_t registers[128];
    uint8_t identity;
    uint8_t fifo_tx[32];
    size_t fifo_tx_len;
    uint8_t response[32];
    size_t response_len;
    uint8_t uid[ST25R3916_UID_MAX];
    size_t uid_len;
    int64_t time_ms;
    unsigned reqa_count;
    unsigned anticollision_count;
    unsigned select_count;
} fake_chip_t;

static void set_response(fake_chip_t *chip, const uint8_t *data, size_t len)
{
    memcpy(chip->response, data, len);
    chip->response_len = len;
}

static esp_err_t fake_transfer(void *ctx,
                               const uint8_t *tx,
                               uint8_t *rx,
                               size_t len)
{
    fake_chip_t *chip = ctx;
    memset(rx, 0, len);
    if (len == 1U) {
        if (tx[0] == CMD_TRANSMIT_REQA) {
            const uint8_t atqa[] = {0x04U, 0x00U};
            set_response(chip, atqa, sizeof(atqa));
            chip->reqa_count++;
        } else if (tx[0] == CMD_TRANSMIT_WITHOUT_CRC) {
            assert(chip->fifo_tx_len == 2U);
            assert(chip->fifo_tx[1] == 0x20U);
            uint8_t response[5];
            const size_t cascade = chip->anticollision_count;
            const bool more = chip->uid_len > 4U &&
                cascade < (chip->uid_len == 7U ? 1U : 2U);
            const size_t uid_offset = cascade * 3U;
            if (more) {
                response[0] = 0x88U;
                memcpy(&response[1], &chip->uid[uid_offset], 3U);
            } else {
                memcpy(response, &chip->uid[uid_offset], 4U);
            }
            response[4] = response[0] ^ response[1] ^ response[2] ^ response[3];
            set_response(chip, response, sizeof(response));
            chip->anticollision_count++;
        } else if (tx[0] == CMD_TRANSMIT_WITH_CRC) {
            assert(chip->fifo_tx_len == 7U);
            assert(chip->fifo_tx[1] == 0x70U);
            const bool more = chip->uid_len > 4U &&
                chip->select_count < (chip->uid_len == 7U ? 1U : 2U);
            const uint8_t sak[] = {more ? 0x04U : 0x00U};
            set_response(chip, sak, sizeof(sak));
            chip->select_count++;
        }
        return ESP_OK;
    }
    if (tx[0] == FIFO_WRITE) {
        chip->fifo_tx_len = len - 1U;
        memcpy(chip->fifo_tx, &tx[1], chip->fifo_tx_len);
        return ESP_OK;
    }
    if (tx[0] == FIFO_READ) {
        assert(len == chip->response_len + 1U);
        memcpy(&rx[1], chip->response, chip->response_len);
        chip->response_len = 0U;
        return ESP_OK;
    }

    size_t offset = 0U;
    bool space_b = false;
    if (tx[offset] == SPACE_B_ACCESS) {
        space_b = true;
        offset++;
    }
    const uint8_t command = tx[offset];
    const uint8_t reg = (space_b ? 0x40U : 0U) | (command & 0x3FU);
    if ((command & READ_REGISTER) != 0U) {
        uint8_t value = chip->registers[reg];
        if (reg == 0x3FU) {
            value = chip->identity;
        } else if (reg == 0x31U) {
            value = 0x10U;
        } else if (reg == 0x1AU && chip->response_len > 0U) {
            value = 0x10U;
        } else if (reg == 0x1EU) {
            value = (uint8_t)chip->response_len;
        }
        rx[offset + 1U] = value;
    } else {
        chip->registers[reg] = tx[offset + 1U];
    }
    return ESP_OK;
}

static void fake_delay(void *ctx, uint32_t delay_ms)
{
    ((fake_chip_t *)ctx)->time_ms += delay_ms;
}

static int64_t fake_time(void *ctx)
{
    return ((fake_chip_t *)ctx)->time_ms;
}

static bool fake_irq(void *ctx)
{
    return ((fake_chip_t *)ctx)->response_len > 0U;
}

static esp_err_t init(fake_chip_t *chip, uint8_t identity, st25r3916_t *device)
{
    memset(chip, 0, sizeof(*chip));
    chip->identity = identity;
    const uint8_t uid[] = {
        0x04U, 0x25U, 0x85U, 0x93U, 0x11U,
        0x22U, 0x33U, 0x44U, 0x55U, 0x66U,
    };
    memcpy(chip->uid, uid, sizeof(uid));
    chip->uid_len = 4U;
    const st25r3916_io_t io = {
        .transfer = fake_transfer,
        .delay_ms = fake_delay,
        .time_ms = fake_time,
        .irq_asserted = fake_irq,
        .ctx = chip,
    };
    return st25r3916_init(device, &io);
}

static void test_chip_specific_configuration(void)
{
    fake_chip_t chip;
    st25r3916_t device;
    assert(init(&chip, 0x28U, &device) == ESP_OK);
    assert(chip.registers[0x26U] == 0x82U);
    assert(chip.registers[0x27U] == 0x82U);
    assert(chip.registers[0x0CU] == 0x2DU);
    assert(chip.registers[0x70U] == 0x40U);

    assert(init(&chip, 0x31U, &device) == ESP_OK);
    assert(chip.registers[0x26U] == 0xC5U);
    assert(chip.registers[0x27U] == 0xE3U);
    assert(chip.registers[0x0CU] == 0xEDU);
    assert(chip.registers[0x6EU] == 0x09U);

    assert(init(&chip, 0x30U, &device) == ESP_ERR_NOT_FOUND);
    assert(init(&chip, 0x20U, &device) == ESP_ERR_NOT_FOUND);
}

static void check_nfca_uid_scan(size_t uid_len)
{
    fake_chip_t chip;
    st25r3916_t device;
    assert(init(&chip, 0x31U, &device) == ESP_OK);
    chip.uid_len = uid_len;
    st25r3916_nfca_tag_t tag;
    assert(st25r3916_nfca_scan(&device, 100U, &tag) == ESP_OK);
    assert(tag.uid_len == uid_len);
    assert(memcmp(tag.uid, chip.uid, uid_len) == 0);
    assert(tag.atqa[0] == 0x04U && tag.atqa[1] == 0x00U);
    assert(tag.sak == 0x00U);
    assert(chip.reqa_count == 1U);
    const unsigned expected_cascades = uid_len == 4U ? 1U : (uid_len == 7U ? 2U : 3U);
    assert(chip.anticollision_count == expected_cascades);
    assert(chip.select_count == expected_cascades);
}

int main(void)
{
    test_chip_specific_configuration();
    check_nfca_uid_scan(4U);
    check_nfca_uid_scan(7U);
    check_nfca_uid_scan(10U);
    puts("st25r3916 tests: ok");
    return 0;
}
