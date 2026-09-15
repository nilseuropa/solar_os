#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BQ25896_INPUT_CURRENT_MIN_MA 100U
#define BQ25896_INPUT_CURRENT_MAX_MA 3250U
#define BQ25896_INPUT_CURRENT_STEP_MA 50U
#define BQ25896_CHARGE_CURRENT_MIN_MA 0U
#define BQ25896_CHARGE_CURRENT_MAX_MA 3008U
#define BQ25896_CHARGE_CURRENT_STEP_MA 64U
#define BQ25896_CHARGE_VOLTAGE_MIN_MV 3840U
#define BQ25896_CHARGE_VOLTAGE_MAX_MV 4608U
#define BQ25896_CHARGE_VOLTAGE_STEP_MV 16U

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    void *ctx;
} bq25896_io_t;

typedef enum {
    BQ25896_CHARGE_STATE_NONE,
    BQ25896_CHARGE_STATE_PRECHARGE,
    BQ25896_CHARGE_STATE_FAST,
    BQ25896_CHARGE_STATE_DONE,
} bq25896_charge_state_t;

typedef struct {
    bool enabled;
    bool input_present;
    bool power_good;
    bq25896_charge_state_t state;
    uint16_t input_current_limit_ma;
    uint16_t charge_current_ma;
    uint16_t charge_voltage_mv;
    uint8_t fault;
} bq25896_status_t;

typedef struct {
    bq25896_io_t io;
    bool initialized;
    uint8_t part_number;
    uint8_t revision;
} bq25896_t;

esp_err_t bq25896_init(bq25896_t *device, const bq25896_io_t *io);
esp_err_t bq25896_deinit(bq25896_t *device);
esp_err_t bq25896_read_status(bq25896_t *device, bq25896_status_t *status);
esp_err_t bq25896_set_enabled(bq25896_t *device, bool enabled);
esp_err_t bq25896_set_input_current_limit(bq25896_t *device,
                                         uint16_t current_ma);
esp_err_t bq25896_set_charge_current(bq25896_t *device, uint16_t current_ma);
esp_err_t bq25896_set_charge_voltage(bq25896_t *device, uint16_t voltage_mv);
