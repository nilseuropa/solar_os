#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bq25896.h"

typedef struct { uint8_t registers[0x15]; } fake_chip_t;

static esp_err_t read_regs(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    fake_chip_t *chip = ctx;
    if ((size_t)reg + len > sizeof(chip->registers)) return ESP_ERR_INVALID_ARG;
    memcpy(data, &chip->registers[reg], len);
    return ESP_OK;
}

static esp_err_t write_regs(void *ctx,
                            uint8_t reg,
                            const uint8_t *data,
                            size_t len)
{
    fake_chip_t *chip = ctx;
    if ((size_t)reg + len > sizeof(chip->registers)) return ESP_ERR_INVALID_ARG;
    memcpy(&chip->registers[reg], data, len);
    return ESP_OK;
}

static esp_err_t init(fake_chip_t *chip, uint8_t identity, bq25896_t *device)
{
    chip->registers[0x14] = identity;
    const bq25896_io_t io = {
        .read = read_regs,
        .write = write_regs,
        .ctx = chip,
    };
    return bq25896_init(device, &io);
}

int main(void)
{
    fake_chip_t chip = {0};
    bq25896_t device;
    chip.registers[0x07] = 0xBF;
    assert(init(&chip, 0x1AU, &device) == ESP_OK);
    assert(device.part_number == 3U);
    assert(device.revision == 2U);
    assert(chip.registers[0x07] == 0x8FU);

    assert(bq25896_set_input_current_limit(&device, 350U) == ESP_OK);
    assert((chip.registers[0x00] & 0x3FU) == 5U);
    assert(bq25896_set_input_current_limit(&device, 351U) == ESP_ERR_INVALID_ARG);
    assert(bq25896_set_charge_current(&device, 704U) == ESP_OK);
    assert((chip.registers[0x04] & 0x7FU) == 11U);
    assert(bq25896_set_charge_current(&device, 705U) == ESP_ERR_INVALID_ARG);
    assert(bq25896_set_charge_voltage(&device, 4288U) == ESP_OK);
    assert((chip.registers[0x06] & 0xFCU) == 0x70U);
    assert(bq25896_set_enabled(&device, true) == ESP_OK);
    assert((chip.registers[0x03] & 0x10U) != 0U);

    chip.registers[0x0B] = (2U << 5) | (2U << 3) | 0x04U;
    chip.registers[0x0C] = 0x08U;
    bq25896_status_t status;
    assert(bq25896_read_status(&device, &status) == ESP_OK);
    assert(status.enabled);
    assert(status.input_present);
    assert(status.power_good);
    assert(status.state == BQ25896_CHARGE_STATE_FAST);
    assert(status.input_current_limit_ma == 350U);
    assert(status.charge_current_ma == 704U);
    assert(status.charge_voltage_mv == 4288U);
    assert(status.fault == 0x08U);
    assert(bq25896_deinit(&device) == ESP_OK);

    memset(&chip, 0, sizeof(chip));
    assert(init(&chip, 0x19U, &device) == ESP_ERR_NOT_FOUND);
    puts("bq25896 tests: ok");
    return 0;
}
