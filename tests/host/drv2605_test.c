#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drv2605.h"

#define REG_STATUS 0x00U
#define REG_MODE 0x01U
#define REG_LIBRARY 0x03U
#define REG_WAVESEQ1 0x04U
#define REG_WAVESEQ2 0x05U
#define REG_GO 0x0CU
#define REG_FEEDBACK 0x1AU

typedef struct {
    uint8_t registers[0x23];
    uint32_t delay_ms;
    int fail_write_reg;
} fake_chip_t;

static esp_err_t fake_read(void *ctx,
                           uint8_t reg,
                           uint8_t *data,
                           size_t len)
{
    fake_chip_t *chip = ctx;
    if ((size_t)reg + len > sizeof(chip->registers)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(data, &chip->registers[reg], len);
    return ESP_OK;
}

static esp_err_t fake_write(void *ctx,
                            uint8_t reg,
                            const uint8_t *data,
                            size_t len)
{
    fake_chip_t *chip = ctx;
    if ((size_t)reg + len > sizeof(chip->registers)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (chip->fail_write_reg == reg) {
        return ESP_FAIL;
    }
    memcpy(&chip->registers[reg], data, len);
    return ESP_OK;
}

static void fake_delay(void *ctx, uint32_t delay_ms)
{
    ((fake_chip_t *)ctx)->delay_ms += delay_ms;
}

static esp_err_t init(fake_chip_t *chip, uint8_t chip_id, drv2605_t *device)
{
    memset(chip, 0, sizeof(*chip));
    chip->registers[REG_STATUS] = (uint8_t)(chip_id << 5U);
    chip->registers[REG_FEEDBACK] = 0xFFU;
    chip->fail_write_reg = -1;
    const drv2605_io_t io = {
        .read = fake_read,
        .write = fake_write,
        .delay_ms = fake_delay,
        .ctx = chip,
    };
    return drv2605_init(device, &io);
}

int main(void)
{
    fake_chip_t chip;
    drv2605_t device;
    assert(init(&chip, 3U, &device) == ESP_OK);
    assert(device.initialized && device.chip_id == 3U);
    assert(chip.registers[REG_MODE] == 0U);
    assert(chip.registers[REG_LIBRARY] == 1U);
    assert(chip.registers[REG_FEEDBACK] == 0x7FU);
    assert(chip.registers[REG_WAVESEQ1] == 0U);
    assert(chip.registers[REG_WAVESEQ2] == 0U);
    assert(chip.registers[REG_GO] == 0U);
    assert(chip.delay_ms == 1U);

    assert(drv2605_play_effect(&device, 0U) == ESP_ERR_INVALID_ARG);
    assert(drv2605_play_effect(&device, DRV2605_EFFECT_COUNT + 1U) ==
           ESP_ERR_INVALID_ARG);
    assert(drv2605_play_effect(&device, 15U) == ESP_OK);
    assert(chip.registers[REG_WAVESEQ1] == 15U);
    assert(chip.registers[REG_WAVESEQ2] == 0U);
    assert(chip.registers[REG_GO] == 1U);
    assert(drv2605_stop(&device) == ESP_OK);
    assert(chip.registers[REG_GO] == 0U);
    assert(drv2605_deinit(&device) == ESP_OK);
    assert(!device.initialized);

    assert(init(&chip, 7U, &device) == ESP_OK);
    assert(init(&chip, 5U, &device) == ESP_OK);
    assert(init(&chip, 4U, &device) == ESP_ERR_NOT_FOUND);
    assert(!device.initialized);

    puts("drv2605 tests: ok");
    return 0;
}
