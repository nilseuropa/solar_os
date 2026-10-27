#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "axp2101.h"

typedef struct {
    uint8_t registers[256];
    esp_err_t read_result;
    esp_err_t write_result;
} fake_chip_t;

static esp_err_t read_regs(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    fake_chip_t *chip = ctx;
    if (chip->read_result != ESP_OK) return chip->read_result;
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
    if (chip->write_result != ESP_OK) return chip->write_result;
    if ((size_t)reg + len > sizeof(chip->registers)) return ESP_ERR_INVALID_ARG;
    memcpy(&chip->registers[reg], data, len);
    return ESP_OK;
}

static esp_err_t init(fake_chip_t *chip, uint8_t identity, axp2101_t *device)
{
    chip->registers[0x03] = identity;
    const axp2101_io_t io = {
        .read = read_regs,
        .write = write_regs,
        .ctx = chip,
    };
    return axp2101_init(device, &io);
}

int main(void)
{
    fake_chip_t chip = {0};
    axp2101_t device;
    assert(init(&chip, 0x00U, &device) == ESP_ERR_NOT_FOUND);
    assert(!device.initialized);

    memset(&chip, 0, sizeof(chip));
    assert(init(&chip, AXP2101_CHIP_ID, &device) == ESP_OK);

    chip.registers[0x68] = 0xA0U;
    chip.registers[0x30] = 0xC2U;
    chip.registers[0xA2] = 0xB0U;
    assert(axp2101_enable_monitoring(&device) == ESP_OK);
    assert(chip.registers[0x68] == 0xA1U);
    assert(chip.registers[0x30] == 0xDFU);
    assert(chip.registers[0xA2] == 0xA1U);

    chip.registers[0x00] = (1U << 5) | (1U << 3) | 0x01U;
    chip.registers[0x01] = (1U << 5) | 2U;
    chip.registers[0x34] = 0x0FU;
    chip.registers[0x35] = 0x50U;
    chip.registers[0xA4] = 73U;
    axp2101_battery_status_t battery;
    assert(axp2101_read_battery(&device, &battery) == ESP_OK);
    assert(battery.battery_present);
    assert(battery.battery_mv == 3920U);
    assert(battery.percent_valid);
    assert(battery.percent == 73U);
    assert(battery.external_power);
    assert(battery.charging);

    chip.registers[0x16] = 0xACU;
    chip.registers[0x18] = 0xE2U;
    chip.registers[0x62] = 0xA8U;
    chip.registers[0x64] = 0xC3U;
    axp2101_charger_status_t charger;
    assert(axp2101_read_charger(&device, &charger) == ESP_OK);
    assert(charger.enabled);
    assert(charger.input_present);
    assert(charger.power_good);
    assert(charger.state == AXP2101_CHARGE_STATE_CONSTANT_CURRENT);
    assert(charger.input_current_limit_ma == 1500U);
    assert(charger.charge_current_ma == 200U);
    assert(charger.charge_voltage_mv == 4200U);
    assert(charger.fault == 1U);

    assert(axp2101_set_input_current_limit(&device, 900U) == ESP_OK);
    assert(chip.registers[0x16] == 0xAAU);
    assert(axp2101_set_input_current_limit(&device, 750U) == ESP_ERR_INVALID_ARG);
    assert(axp2101_set_charge_current(&device, 500U) == ESP_OK);
    assert(chip.registers[0x62] == 0xABU);
    assert(axp2101_set_charge_current(&device, 250U) == ESP_ERR_INVALID_ARG);
    assert(axp2101_set_charge_voltage(&device, 4100U) == ESP_OK);
    assert(chip.registers[0x64] == 0xC2U);
    assert(axp2101_set_charge_voltage(&device, 4150U) == ESP_ERR_INVALID_ARG);
    assert(axp2101_set_charger_enabled(&device, false) == ESP_OK);
    assert(chip.registers[0x18] == 0xE0U);

    chip.registers[0x94] = 0xE0U;
    chip.registers[0x90] = 0xA0U;
    assert(axp2101_set_aldo3(&device, true, 3300U) == ESP_OK);
    assert(chip.registers[0x94] == 0xFCU);
    assert(chip.registers[0x90] == 0xA4U);
    assert(axp2101_set_aldo3(&device, false, 3300U) == ESP_OK);
    assert(chip.registers[0x90] == 0xA0U);
    assert(axp2101_set_aldo3(&device, true, 3350U) == ESP_ERR_INVALID_ARG);

    chip.registers[0x10] = 0xA4U;
    assert(axp2101_power_off(&device) == ESP_OK);
    assert(chip.registers[0x10] == 0xA5U);
    chip.read_result = ESP_ERR_TIMEOUT;
    assert(axp2101_power_off(&device) == ESP_ERR_TIMEOUT);
    chip.read_result = ESP_OK;
    chip.write_result = ESP_FAIL;
    assert(axp2101_power_off(&device) == ESP_FAIL);
    chip.write_result = ESP_OK;
    assert(axp2101_deinit(&device) == ESP_OK);
    assert(axp2101_power_off(&device) == ESP_ERR_INVALID_STATE);
    assert(axp2101_read_battery(&device, &battery) == ESP_ERR_INVALID_ARG);
    puts("AXP2101 tests: ok");
    return 0;
}
