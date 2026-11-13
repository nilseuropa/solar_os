#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SY6974_INPUT_CURRENT_MIN_MA 100U
#define SY6974_INPUT_CURRENT_MAX_MA 3200U
#define SY6974_INPUT_CURRENT_STEP_MA 100U
#define SY6974_CHARGE_CURRENT_MIN_MA 0U
#define SY6974_CHARGE_CURRENT_MAX_MA 3000U
#define SY6974_CHARGE_CURRENT_STEP_MA 60U
#define SY6974_CHARGE_VOLTAGE_MIN_MV 3856U
#define SY6974_CHARGE_VOLTAGE_MAX_MV 4624U
#define SY6974_CHARGE_VOLTAGE_STEP_MV 32U

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    void *ctx;
} sy6974_io_t;

typedef enum {
    SY6974_CHARGE_STATE_NONE,
    SY6974_CHARGE_STATE_PRECHARGE,
    SY6974_CHARGE_STATE_FAST,
    SY6974_CHARGE_STATE_DONE,
} sy6974_charge_state_t;

typedef struct {
    bool enabled;
    bool input_present;
    bool power_good;
    sy6974_charge_state_t state;
    uint16_t input_current_limit_ma;
    uint16_t charge_current_ma;
    uint16_t charge_voltage_mv;
    uint8_t fault;
} sy6974_status_t;

typedef struct {
    sy6974_io_t io;
    bool initialized;
    uint8_t part_number;
    uint8_t revision;
} sy6974_t;

esp_err_t sy6974_init(sy6974_t *device, const sy6974_io_t *io);
esp_err_t sy6974_deinit(sy6974_t *device);
esp_err_t sy6974_read_status(sy6974_t *device, sy6974_status_t *status);
esp_err_t sy6974_set_enabled(sy6974_t *device, bool enabled);
esp_err_t sy6974_set_input_current_limit(sy6974_t *device,
                                         uint16_t current_ma);
esp_err_t sy6974_set_charge_current(sy6974_t *device, uint16_t current_ma);
esp_err_t sy6974_set_charge_voltage(sy6974_t *device, uint16_t voltage_mv);
esp_err_t sy6974_set_input_voltage_limit(sy6974_t *device, uint16_t voltage_mv);
