#include "axp2101.h"

#include <string.h>

#define REG_STATUS1 0x00U
#define REG_STATUS2 0x01U
#define REG_CHIP_ID 0x03U
#define REG_COMMON_CONFIG 0x10U
#define REG_CHARGE_GAUGE_WDT 0x18U
#define REG_INPUT_CURRENT_LIMIT 0x16U
#define REG_ADC_CHANNEL 0x30U
#define REG_BATTERY_VOLTAGE_H 0x34U
#define REG_CHARGE_CURRENT 0x62U
#define REG_CHARGE_VOLTAGE 0x64U
#define REG_BATTERY_DETECT 0x68U
#define REG_LDO_ENABLE0 0x90U
#define REG_ALDO3_VOLTAGE 0x94U
#define REG_FUEL_GAUGE_CONTROL 0xA2U
#define REG_BATTERY_PERCENT 0xA4U

#define STATUS1_VBUS_GOOD (1U << 5)
#define STATUS1_BATTERY_PRESENT (1U << 3)
#define STATUS1_FAULT_MASK 0x03U
#define STATUS2_MODE_MASK 0xE0U
#define STATUS2_MODE_CHARGING 0x20U
#define STATUS2_VBUS_REMOVED (1U << 3)
#define STATUS2_CHARGE_STATE_MASK 0x07U
#define CELL_BATTERY_CHARGE_ENABLE (1U << 1)
#define ADC_MONITOR_MASK ((1U << 4) | (1U << 3) | (1U << 2) | (1U << 0))
#define BATTERY_DETECT_ENABLE (1U << 0)
#define FUEL_GAUGE_ENABLE (1U << 0)
#define FUEL_GAUGE_WRITE_ROM (1U << 4)
#define ALDO3_ENABLE (1U << 2)

static const uint16_t input_current_values[] = {
    100U, 500U, 900U, 1000U, 1500U, 2000U,
};

static const uint16_t charge_current_values[] = {
    0U, 0U, 0U, 0U, 100U, 125U, 150U, 175U, 200U,
    300U, 400U, 500U, 600U, 700U, 800U, 900U, 1000U,
};

static const uint16_t charge_voltage_values[] = {
    0U, 4000U, 4100U, 4200U, 4350U, 4400U,
};

static bool device_valid(const axp2101_t *device)
{
    return device != NULL && device->initialized &&
        device->io.read != NULL && device->io.write != NULL;
}

static esp_err_t read_u8(axp2101_t *device, uint8_t reg, uint8_t *value)
{
    return device->io.read(device->io.ctx, reg, value, 1U);
}

static esp_err_t write_u8(axp2101_t *device, uint8_t reg, uint8_t value)
{
    return device->io.write(device->io.ctx, reg, &value, 1U);
}

static esp_err_t update_bits(axp2101_t *device,
                             uint8_t reg,
                             uint8_t mask,
                             uint8_t value)
{
    uint8_t current = 0U;
    esp_err_t ret = read_u8(device, reg, &current);
    if (ret != ESP_OK) return ret;
    current = (uint8_t)((current & (uint8_t)~mask) | (value & mask));
    return write_u8(device, reg, current);
}

static int find_value(const uint16_t *values, size_t count, uint16_t value)
{
    for (size_t i = 0; i < count; i++) {
        if (values[i] == value) return (int)i;
    }
    return -1;
}

esp_err_t axp2101_init(axp2101_t *device, const axp2101_io_t *io)
{
    if (device == NULL || io == NULL || io->read == NULL || io->write == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    device->io = *io;
    uint8_t chip_id = 0U;
    const esp_err_t ret = read_u8(device, REG_CHIP_ID, &chip_id);
    if (ret != ESP_OK || chip_id != AXP2101_CHIP_ID) {
        memset(device, 0, sizeof(*device));
        return ret != ESP_OK ? ret : ESP_ERR_NOT_FOUND;
    }
    device->initialized = true;
    return ESP_OK;
}

esp_err_t axp2101_power_off(axp2101_t *device)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    return update_bits(device, REG_COMMON_CONFIG, 1U, 1U);
}

esp_err_t axp2101_deinit(axp2101_t *device)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    memset(device, 0, sizeof(*device));
    return ESP_OK;
}

esp_err_t axp2101_enable_monitoring(axp2101_t *device)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = update_bits(device, REG_BATTERY_DETECT,
                                BATTERY_DETECT_ENABLE,
                                BATTERY_DETECT_ENABLE);
    if (ret == ESP_OK) {
        ret = update_bits(device, REG_ADC_CHANNEL,
                          ADC_MONITOR_MASK, ADC_MONITOR_MASK);
    }
    if (ret == ESP_OK) {
        ret = update_bits(device, REG_FUEL_GAUGE_CONTROL,
                          FUEL_GAUGE_WRITE_ROM | FUEL_GAUGE_ENABLE,
                          FUEL_GAUGE_ENABLE);
    }
    return ret;
}

esp_err_t axp2101_read_battery(axp2101_t *device,
                               axp2101_battery_status_t *status)
{
    if (!device_valid(device) || status == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t power_status[2] = {0};
    esp_err_t ret = device->io.read(device->io.ctx, REG_STATUS1,
                                    power_status, sizeof(power_status));
    if (ret != ESP_OK) return ret;
    memset(status, 0, sizeof(*status));
    status->battery_present = (power_status[0] & STATUS1_BATTERY_PRESENT) != 0U;
    status->external_power =
        (power_status[0] & STATUS1_VBUS_GOOD) != 0U &&
        (power_status[1] & STATUS2_VBUS_REMOVED) == 0U;
    status->charging =
        (power_status[1] & STATUS2_MODE_MASK) == STATUS2_MODE_CHARGING;
    if (!status->battery_present) return ESP_OK;

    uint8_t voltage[2] = {0};
    ret = device->io.read(device->io.ctx, REG_BATTERY_VOLTAGE_H,
                          voltage, sizeof(voltage));
    if (ret != ESP_OK) return ret;
    status->battery_mv = (uint16_t)(((uint16_t)(voltage[0] & 0x1FU) << 8) |
                                    voltage[1]);
    uint8_t percent = 0U;
    ret = read_u8(device, REG_BATTERY_PERCENT, &percent);
    if (ret != ESP_OK) return ret;
    status->percent_valid = percent <= 100U;
    status->percent = status->percent_valid ? percent : 0U;
    return ESP_OK;
}

esp_err_t axp2101_read_charger(axp2101_t *device,
                               axp2101_charger_status_t *status)
{
    if (!device_valid(device) || status == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t power_status[2] = {0};
    uint8_t input = 0U;
    uint8_t control = 0U;
    uint8_t current = 0U;
    uint8_t voltage = 0U;
    esp_err_t ret = device->io.read(device->io.ctx, REG_STATUS1,
                                    power_status, sizeof(power_status));
    if (ret == ESP_OK) ret = read_u8(device, REG_INPUT_CURRENT_LIMIT, &input);
    if (ret == ESP_OK) ret = read_u8(device, REG_CHARGE_GAUGE_WDT, &control);
    if (ret == ESP_OK) ret = read_u8(device, REG_CHARGE_CURRENT, &current);
    if (ret == ESP_OK) ret = read_u8(device, REG_CHARGE_VOLTAGE, &voltage);
    if (ret != ESP_OK) return ret;

    const uint8_t state_code = power_status[1] & STATUS2_CHARGE_STATE_MASK;
    axp2101_charge_state_t state = AXP2101_CHARGE_STATE_UNKNOWN;
    if (state_code <= AXP2101_CHARGE_STATE_STOPPED) {
        state = (axp2101_charge_state_t)state_code;
    }
    const uint8_t input_code = input & 0x07U;
    const uint8_t current_code = current & 0x1FU;
    const uint8_t voltage_code = voltage & 0x07U;
    *status = (axp2101_charger_status_t) {
        .enabled = (control & CELL_BATTERY_CHARGE_ENABLE) != 0U,
        .input_present =
            (power_status[0] & STATUS1_VBUS_GOOD) != 0U &&
            (power_status[1] & STATUS2_VBUS_REMOVED) == 0U,
        .power_good = (power_status[0] & STATUS1_VBUS_GOOD) != 0U,
        .state = state,
        .input_current_limit_ma =
            input_code < sizeof(input_current_values) / sizeof(input_current_values[0]) ?
                input_current_values[input_code] : 0U,
        .charge_current_ma =
            current_code < sizeof(charge_current_values) / sizeof(charge_current_values[0]) ?
                charge_current_values[current_code] : 0U,
        .charge_voltage_mv =
            voltage_code < sizeof(charge_voltage_values) / sizeof(charge_voltage_values[0]) ?
                charge_voltage_values[voltage_code] : 0U,
        .fault = power_status[0] & STATUS1_FAULT_MASK,
    };
    return ESP_OK;
}

esp_err_t axp2101_set_charger_enabled(axp2101_t *device, bool enabled)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    return update_bits(device, REG_CHARGE_GAUGE_WDT,
                       CELL_BATTERY_CHARGE_ENABLE,
                       enabled ? CELL_BATTERY_CHARGE_ENABLE : 0U);
}

esp_err_t axp2101_set_input_current_limit(axp2101_t *device,
                                          uint16_t current_ma)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    const int code = find_value(input_current_values,
                                sizeof(input_current_values) /
                                    sizeof(input_current_values[0]),
                                current_ma);
    return code >= 0 ? update_bits(device, REG_INPUT_CURRENT_LIMIT,
                                   0x07U, (uint8_t)code) : ESP_ERR_INVALID_ARG;
}

esp_err_t axp2101_set_charge_current(axp2101_t *device,
                                     uint16_t current_ma)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    const int code = find_value(charge_current_values,
                                sizeof(charge_current_values) /
                                    sizeof(charge_current_values[0]),
                                current_ma);
    return code >= 0 ? update_bits(device, REG_CHARGE_CURRENT,
                                   0x1FU, (uint8_t)code) : ESP_ERR_INVALID_ARG;
}

esp_err_t axp2101_set_charge_voltage(axp2101_t *device,
                                     uint16_t voltage_mv)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    const int code = find_value(charge_voltage_values,
                                sizeof(charge_voltage_values) /
                                    sizeof(charge_voltage_values[0]),
                                voltage_mv);
    return code >= 0 ? update_bits(device, REG_CHARGE_VOLTAGE,
                                   0x07U, (uint8_t)code) : ESP_ERR_INVALID_ARG;
}

esp_err_t axp2101_set_aldo3(axp2101_t *device,
                            bool enabled,
                            uint16_t voltage_mv)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    if (voltage_mv < 500U || voltage_mv > 3500U ||
        (voltage_mv - 500U) % 100U != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;
    if (enabled) {
        ret = update_bits(device, REG_ALDO3_VOLTAGE, 0x1FU,
                          (uint8_t)((voltage_mv - 500U) / 100U));
    }
    return ret == ESP_OK ? update_bits(device, REG_LDO_ENABLE0,
                                       ALDO3_ENABLE,
                                       enabled ? ALDO3_ENABLE : 0U) : ret;
}
