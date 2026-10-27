#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AXP2101_I2C_ADDRESS 0x34U
#define AXP2101_CHIP_ID 0x4AU

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    void *ctx;
} axp2101_io_t;

typedef enum {
    AXP2101_CHARGE_STATE_TRICKLE,
    AXP2101_CHARGE_STATE_PRECHARGE,
    AXP2101_CHARGE_STATE_CONSTANT_CURRENT,
    AXP2101_CHARGE_STATE_CONSTANT_VOLTAGE,
    AXP2101_CHARGE_STATE_DONE,
    AXP2101_CHARGE_STATE_STOPPED,
    AXP2101_CHARGE_STATE_UNKNOWN,
} axp2101_charge_state_t;

typedef struct {
    bool battery_present;
    uint16_t battery_mv;
    bool percent_valid;
    uint8_t percent;
    bool external_power;
    bool charging;
} axp2101_battery_status_t;

typedef struct {
    bool enabled;
    bool input_present;
    bool power_good;
    axp2101_charge_state_t state;
    uint16_t input_current_limit_ma;
    uint16_t charge_current_ma;
    uint16_t charge_voltage_mv;
    uint8_t fault;
} axp2101_charger_status_t;

typedef struct {
    axp2101_io_t io;
    bool initialized;
} axp2101_t;

esp_err_t axp2101_init(axp2101_t *device, const axp2101_io_t *io);
esp_err_t axp2101_deinit(axp2101_t *device);
esp_err_t axp2101_power_off(axp2101_t *device);
esp_err_t axp2101_enable_monitoring(axp2101_t *device);
esp_err_t axp2101_read_battery(axp2101_t *device,
                               axp2101_battery_status_t *status);
esp_err_t axp2101_read_charger(axp2101_t *device,
                               axp2101_charger_status_t *status);
esp_err_t axp2101_set_charger_enabled(axp2101_t *device, bool enabled);
esp_err_t axp2101_set_input_current_limit(axp2101_t *device,
                                          uint16_t current_ma);
esp_err_t axp2101_set_charge_current(axp2101_t *device,
                                     uint16_t current_ma);
esp_err_t axp2101_set_charge_voltage(axp2101_t *device,
                                     uint16_t voltage_mv);
esp_err_t axp2101_set_aldo3(axp2101_t *device,
                            bool enabled,
                            uint16_t voltage_mv);
