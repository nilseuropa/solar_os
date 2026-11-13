/* SY6974B register map: Seeed SY6974 reference and Zephyr charger_sy6974b.
 * Probe part identity before any writes; preserve unrelated register bits. */
#include "sy6974.h"

#include <string.h>

#define REG_INPUT_SOURCE 0x00U
#define REG_CHARGE_CONTROL 0x01U
#define REG_CHARGE_CURRENT 0x02U
#define REG_CHARGE_VOLTAGE 0x04U
#define REG_CHARGE_TIMER 0x05U
#define REG_INPUT_VOLTAGE 0x06U
#define REG_STATUS 0x08U
#define REG_FAULT 0x09U
#define REG_DEVICE 0x0BU

#define INPUT_CURRENT_MASK 0x1FU
#define CHARGE_ENABLE_MASK 0x10U
#define CHARGE_CURRENT_MASK 0x3FU
#define CHARGE_VOLTAGE_MASK 0xF8U
#define WATCHDOG_MASK 0x30U
#define VBUS_STATUS_MASK 0xE0U
#define CHARGE_STATUS_MASK 0x18U
#define POWER_GOOD_MASK 0x04U
#define DEVICE_PART_MASK 0x78U
#define DEVICE_REVISION_MASK 0x03U
#define DEVICE_PART 0x08U

static bool device_valid(const sy6974_t *device)
{
    return device != NULL && device->initialized &&
        device->io.read != NULL && device->io.write != NULL;
}

static esp_err_t read_u8(sy6974_t *device, uint8_t reg, uint8_t *value)
{
    return device->io.read(device->io.ctx, reg, value, 1U);
}

static esp_err_t write_u8(sy6974_t *device, uint8_t reg, uint8_t value)
{
    return device->io.write(device->io.ctx, reg, &value, 1U);
}

static esp_err_t update_bits(sy6974_t *device,
                             uint8_t reg,
                             uint8_t mask,
                             uint8_t value)
{
    uint8_t current;
    esp_err_t ret = read_u8(device, reg, &current);
    if (ret != ESP_OK) {
        return ret;
    }
    current = (uint8_t)((current & (uint8_t)~mask) | (value & mask));
    return write_u8(device, reg, current);
}

static bool exact(uint16_t value,
                  uint16_t minimum,
                  uint16_t maximum,
                  uint16_t step)
{
    return value >= minimum && value <= maximum &&
        ((uint32_t)value - minimum) % step == 0U;
}

esp_err_t sy6974_init(sy6974_t *device, const sy6974_io_t *io)
{
    if (device == NULL || io == NULL || io->read == NULL || io->write == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    device->io = *io;
    uint8_t identity;
    esp_err_t ret = read_u8(device, REG_DEVICE, &identity);
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    device->part_number = (identity & DEVICE_PART_MASK) >> 3;
    device->revision = identity & DEVICE_REVISION_MASK;
    if (device->part_number != DEVICE_PART) {
        memset(device, 0, sizeof(*device));
        return ESP_ERR_NOT_FOUND;
    }

    device->initialized = true;
    ret = update_bits(device, REG_CHARGE_TIMER, WATCHDOG_MASK, 0U);
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
    }
    return ret;
}

esp_err_t sy6974_deinit(sy6974_t *device)
{
    if (!device_valid(device)) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(device, 0, sizeof(*device));
    return ESP_OK;
}

esp_err_t sy6974_read_status(sy6974_t *device, sy6974_status_t *status)
{
    if (!device_valid(device) || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t input;
    uint8_t control;
    uint8_t current;
    uint8_t voltage;
    uint8_t state;
    uint8_t fault;
    esp_err_t ret = read_u8(device, REG_INPUT_SOURCE, &input);
    if (ret == ESP_OK) ret = read_u8(device, REG_CHARGE_CONTROL, &control);
    if (ret == ESP_OK) ret = read_u8(device, REG_CHARGE_CURRENT, &current);
    if (ret == ESP_OK) ret = read_u8(device, REG_CHARGE_VOLTAGE, &voltage);
    if (ret == ESP_OK) ret = read_u8(device, REG_STATUS, &state);
    if (ret == ESP_OK) ret = read_u8(device, REG_FAULT, &fault);
    if (ret != ESP_OK) {
        return ret;
    }
    const uint8_t vbus = (state & VBUS_STATUS_MASK) >> 5;
    *status = (sy6974_status_t) {
        .enabled = (control & CHARGE_ENABLE_MASK) != 0U,
        .input_present = vbus != 0U && vbus != 7U,
        .power_good = (state & POWER_GOOD_MASK) != 0U,
        .state = (sy6974_charge_state_t)((state & CHARGE_STATUS_MASK) >> 3),
        .input_current_limit_ma = SY6974_INPUT_CURRENT_MIN_MA +
            (uint16_t)(input & INPUT_CURRENT_MASK) *
                SY6974_INPUT_CURRENT_STEP_MA,
        .charge_current_ma = (uint16_t)(current & CHARGE_CURRENT_MASK) *
            SY6974_CHARGE_CURRENT_STEP_MA,
        .charge_voltage_mv = SY6974_CHARGE_VOLTAGE_MIN_MV +
            (uint16_t)((voltage & CHARGE_VOLTAGE_MASK) >> 3) *
                SY6974_CHARGE_VOLTAGE_STEP_MV,
        .fault = fault,
    };
    return ESP_OK;
}

esp_err_t sy6974_set_enabled(sy6974_t *device, bool enabled)
{
    if (!device_valid(device)) {
        return ESP_ERR_INVALID_STATE;
    }
    return update_bits(device, REG_CHARGE_CONTROL, CHARGE_ENABLE_MASK,
                       enabled ? CHARGE_ENABLE_MASK : 0U);
}

esp_err_t sy6974_set_input_current_limit(sy6974_t *device,
                                         uint16_t current_ma)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    if (!exact(current_ma, SY6974_INPUT_CURRENT_MIN_MA,
               SY6974_INPUT_CURRENT_MAX_MA,
               SY6974_INPUT_CURRENT_STEP_MA)) return ESP_ERR_INVALID_ARG;
    return update_bits(device, REG_INPUT_SOURCE, INPUT_CURRENT_MASK,
        (uint8_t)((current_ma - SY6974_INPUT_CURRENT_MIN_MA) /
                  SY6974_INPUT_CURRENT_STEP_MA));
}

esp_err_t sy6974_set_charge_current(sy6974_t *device, uint16_t current_ma)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    if (!exact(current_ma, SY6974_CHARGE_CURRENT_MIN_MA,
               SY6974_CHARGE_CURRENT_MAX_MA,
               SY6974_CHARGE_CURRENT_STEP_MA)) return ESP_ERR_INVALID_ARG;
    return update_bits(device, REG_CHARGE_CURRENT, CHARGE_CURRENT_MASK,
        (uint8_t)(current_ma / SY6974_CHARGE_CURRENT_STEP_MA));
}

esp_err_t sy6974_set_charge_voltage(sy6974_t *device, uint16_t voltage_mv)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    if (!exact(voltage_mv, SY6974_CHARGE_VOLTAGE_MIN_MV,
               SY6974_CHARGE_VOLTAGE_MAX_MV,
               SY6974_CHARGE_VOLTAGE_STEP_MV)) return ESP_ERR_INVALID_ARG;
    return update_bits(device, REG_CHARGE_VOLTAGE, CHARGE_VOLTAGE_MASK,
        (uint8_t)(((voltage_mv - SY6974_CHARGE_VOLTAGE_MIN_MV) /
                   SY6974_CHARGE_VOLTAGE_STEP_MV) << 3));
}

esp_err_t sy6974_set_input_voltage_limit(sy6974_t *device, uint16_t voltage_mv)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    if (!exact(voltage_mv, 3900, 5400, 100)) return ESP_ERR_INVALID_ARG;
    return update_bits(device, REG_INPUT_VOLTAGE, 0x0f, (uint8_t)((voltage_mv - 3900) / 100));
}
