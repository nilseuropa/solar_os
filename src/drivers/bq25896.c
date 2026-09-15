#include "bq25896.h"

#include <string.h>

#define REG_INPUT_SOURCE 0x00U
#define REG_CHARGE_CONTROL 0x03U
#define REG_CHARGE_CURRENT 0x04U
#define REG_CHARGE_VOLTAGE 0x06U
#define REG_CHARGE_TIMER 0x07U
#define REG_STATUS 0x0BU
#define REG_FAULT 0x0CU
#define REG_DEVICE 0x14U

#define INPUT_CURRENT_MASK 0x3FU
#define CHARGE_ENABLE_MASK 0x10U
#define CHARGE_CURRENT_MASK 0x7FU
#define CHARGE_VOLTAGE_MASK 0xFCU
#define WATCHDOG_MASK 0x30U
#define VBUS_STATUS_MASK 0xE0U
#define CHARGE_STATUS_MASK 0x18U
#define POWER_GOOD_MASK 0x04U
#define DEVICE_PART_MASK 0x38U
#define DEVICE_REVISION_MASK 0x03U
#define DEVICE_REVISION 0x02U

static bool device_valid(const bq25896_t *device)
{
    return device != NULL && device->initialized &&
        device->io.read != NULL && device->io.write != NULL;
}

static esp_err_t read_u8(bq25896_t *device, uint8_t reg, uint8_t *value)
{
    return device->io.read(device->io.ctx, reg, value, 1U);
}

static esp_err_t write_u8(bq25896_t *device, uint8_t reg, uint8_t value)
{
    return device->io.write(device->io.ctx, reg, &value, 1U);
}

static esp_err_t update_bits(bq25896_t *device,
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

esp_err_t bq25896_init(bq25896_t *device, const bq25896_io_t *io)
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
    if (device->revision != DEVICE_REVISION) {
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

esp_err_t bq25896_deinit(bq25896_t *device)
{
    if (!device_valid(device)) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(device, 0, sizeof(*device));
    return ESP_OK;
}

esp_err_t bq25896_read_status(bq25896_t *device, bq25896_status_t *status)
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
    *status = (bq25896_status_t) {
        .enabled = (control & CHARGE_ENABLE_MASK) != 0U,
        .input_present = vbus != 0U && vbus != 7U,
        .power_good = (state & POWER_GOOD_MASK) != 0U,
        .state = (bq25896_charge_state_t)((state & CHARGE_STATUS_MASK) >> 3),
        .input_current_limit_ma = BQ25896_INPUT_CURRENT_MIN_MA +
            (uint16_t)(input & INPUT_CURRENT_MASK) *
                BQ25896_INPUT_CURRENT_STEP_MA,
        .charge_current_ma = (uint16_t)(current & CHARGE_CURRENT_MASK) *
            BQ25896_CHARGE_CURRENT_STEP_MA,
        .charge_voltage_mv = BQ25896_CHARGE_VOLTAGE_MIN_MV +
            (uint16_t)((voltage & CHARGE_VOLTAGE_MASK) >> 2) *
                BQ25896_CHARGE_VOLTAGE_STEP_MV,
        .fault = fault,
    };
    return ESP_OK;
}

esp_err_t bq25896_set_enabled(bq25896_t *device, bool enabled)
{
    if (!device_valid(device)) {
        return ESP_ERR_INVALID_STATE;
    }
    return update_bits(device, REG_CHARGE_CONTROL, CHARGE_ENABLE_MASK,
                       enabled ? CHARGE_ENABLE_MASK : 0U);
}

esp_err_t bq25896_set_input_current_limit(bq25896_t *device,
                                         uint16_t current_ma)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    if (!exact(current_ma, BQ25896_INPUT_CURRENT_MIN_MA,
               BQ25896_INPUT_CURRENT_MAX_MA,
               BQ25896_INPUT_CURRENT_STEP_MA)) return ESP_ERR_INVALID_ARG;
    return update_bits(device, REG_INPUT_SOURCE, INPUT_CURRENT_MASK,
        (uint8_t)((current_ma - BQ25896_INPUT_CURRENT_MIN_MA) /
                  BQ25896_INPUT_CURRENT_STEP_MA));
}

esp_err_t bq25896_set_charge_current(bq25896_t *device, uint16_t current_ma)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    if (!exact(current_ma, BQ25896_CHARGE_CURRENT_MIN_MA,
               BQ25896_CHARGE_CURRENT_MAX_MA,
               BQ25896_CHARGE_CURRENT_STEP_MA)) return ESP_ERR_INVALID_ARG;
    return update_bits(device, REG_CHARGE_CURRENT, CHARGE_CURRENT_MASK,
        (uint8_t)(current_ma / BQ25896_CHARGE_CURRENT_STEP_MA));
}

esp_err_t bq25896_set_charge_voltage(bq25896_t *device, uint16_t voltage_mv)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    if (!exact(voltage_mv, BQ25896_CHARGE_VOLTAGE_MIN_MV,
               BQ25896_CHARGE_VOLTAGE_MAX_MV,
               BQ25896_CHARGE_VOLTAGE_STEP_MV)) return ESP_ERR_INVALID_ARG;
    return update_bits(device, REG_CHARGE_VOLTAGE, CHARGE_VOLTAGE_MASK,
        (uint8_t)(((voltage_mv - BQ25896_CHARGE_VOLTAGE_MIN_MV) /
                   BQ25896_CHARGE_VOLTAGE_STEP_MV) << 2));
}
