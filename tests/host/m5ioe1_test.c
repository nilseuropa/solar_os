#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "m5ioe1.h"

typedef struct {
    uint8_t registers[0x30];
    uint8_t write_regs[32];
    size_t write_lengths[32];
    size_t write_count;
} fake_io_t;

static esp_err_t fake_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    fake_io_t *fake = ctx;
    memcpy(data, &fake->registers[reg], len);
    return ESP_OK;
}

static esp_err_t fake_write(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    fake_io_t *fake = ctx;
    assert(fake->write_count < 32);
    fake->write_regs[fake->write_count] = reg;
    fake->write_lengths[fake->write_count++] = len;
    memcpy(&fake->registers[reg], data, len);
    return ESP_OK;
}

int main(void)
{
    fake_io_t fake = {0};
    const m5ioe1_io_t io = {
        .read = fake_read,
        .write = fake_write,
        .ctx = &fake,
    };
    m5ioe1_t device;

    /* Power-on defaults: every line open-drain. */
    fake.registers[0x13] = 0xFFU;
    fake.registers[0x14] = 0x3FU;
    assert(m5ioe1_init(&device, &io, 0x3234U, 0x3FBEU) == ESP_OK);
    /* I2C_CFG, OUT_L/OUT_H, DRV_L/DRV_H, then MODE_L/MODE_H. */
    assert(fake.write_count == 7U);
    assert(fake.write_regs[3] == 0x13U && fake.write_regs[4] == 0x14U);
    /* Outputs are push-pull; input lines 0 and 6 keep their drive bits. */
    assert(fake.registers[0x13] == 0x41U && fake.registers[0x14] == 0x00U);
    assert(fake.write_regs[0] == 0x23U);
    assert(fake.registers[0x23] == 0x00U);
    assert(fake.registers[0x05] == 0x34U && fake.registers[0x06] == 0x32U);
    assert(fake.registers[0x03] == 0xBEU && fake.registers[0x04] == 0x3FU);

    assert(m5ioe1_write(&device, 3U, true) == ESP_OK);
    assert((device.output & (1U << 3U)) != 0U);
    fake.registers[0x07] = 0x01U;
    bool level = true;
    assert(m5ioe1_read(&device, 0U, &level) == ESP_OK);
    assert(level);
    assert(m5ioe1_configure(&device, 0U, true) == ESP_OK);
    assert((device.direction & 1U) != 0U);
    assert((fake.registers[0x13] & 0x01U) == 0U);
    assert((fake.registers[0x13] & 0x40U) != 0U);
    assert(m5ioe1_write(&device, M5IOE1_LINE_COUNT, true) == ESP_ERR_INVALID_ARG);
    assert(m5ioe1_deinit(&device) == ESP_OK);
    assert(m5ioe1_read(&device, 0U, &level) == ESP_ERR_INVALID_ARG);
    puts("ok");
    return 0;
}
