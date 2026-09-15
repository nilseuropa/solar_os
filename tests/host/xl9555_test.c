#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "xl9555.h"

typedef struct {
    uint8_t registers[8];
    uint8_t write_regs[8];
    size_t write_count;
} fake_io_t;

static esp_err_t fake_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    fake_io_t *fake = ctx;
    memcpy(data, &fake->registers[reg], len);
    return ESP_OK;
}

static esp_err_t fake_write(void *ctx,
                            uint8_t reg,
                            const uint8_t *data,
                            size_t len)
{
    fake_io_t *fake = ctx;
    assert(fake->write_count < sizeof(fake->write_regs));
    fake->write_regs[fake->write_count++] = reg;
    memcpy(&fake->registers[reg], data, len);
    return ESP_OK;
}

int main(void)
{
    fake_io_t fake = {0};
    const xl9555_io_t io = {
        .read = fake_read,
        .write = fake_write,
        .ctx = &fake,
    };
    xl9555_t device;

    assert(xl9555_init(&device, &io, 0x1234U, 0xFFF0U) == ESP_OK);
    assert(fake.write_count == 2U);
    assert(fake.write_regs[0] == 0x02U);
    assert(fake.write_regs[1] == 0x06U);
    assert(fake.registers[2] == 0x34U && fake.registers[3] == 0x12U);
    assert(fake.registers[6] == 0xF0U && fake.registers[7] == 0xFFU);

    /* A write preloads the latch, then changes an input line to output. */
    assert(xl9555_write(&device, 6U, true) == ESP_OK);
    assert(fake.write_count == 4U);
    assert(fake.write_regs[2] == 0x02U);
    assert(fake.write_regs[3] == 0x06U);
    assert(device.output == 0x1274U);
    assert(device.direction == 0xFFB0U);

    fake.registers[0] = 0x00U;
    fake.registers[1] = 0x02U;
    bool level = false;
    assert(xl9555_read(&device, 9U, &level) == ESP_OK);
    assert(level);
    assert(xl9555_configure(&device, 9U, false) == ESP_OK);
    assert((device.direction & (1U << 9U)) != 0U);
    assert(xl9555_write(&device, XL9555_LINE_COUNT, true) == ESP_ERR_INVALID_ARG);
    assert(xl9555_deinit(&device) == ESP_OK);
    assert(xl9555_read(&device, 0U, &level) == ESP_ERR_INVALID_STATE);

    puts("XL9555 tests passed");
    return 0;
}
