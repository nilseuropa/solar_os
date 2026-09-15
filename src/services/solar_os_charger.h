#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_CHARGER_NAME_MAX 20
#define SOLAR_OS_CHARGER_DRIVER_MAX 24

typedef enum {
    SOLAR_OS_CHARGER_STATE_UNKNOWN,
    SOLAR_OS_CHARGER_STATE_NOT_CHARGING,
    SOLAR_OS_CHARGER_STATE_PRECHARGE,
    SOLAR_OS_CHARGER_STATE_FAST_CHARGE,
    SOLAR_OS_CHARGER_STATE_DONE,
} solar_os_charger_state_t;

typedef struct {
    uint16_t minimum;
    uint16_t maximum;
    uint16_t step;
} solar_os_charger_range_t;

typedef struct {
    bool enabled;
    bool input_present;
    bool power_good;
    solar_os_charger_state_t state;
    uint16_t input_current_limit_ma;
    uint16_t charge_current_ma;
    uint16_t charge_voltage_mv;
    uint8_t fault;
} solar_os_charger_status_t;

typedef struct {
    esp_err_t (*read_status)(void *ctx, solar_os_charger_status_t *status);
    esp_err_t (*set_enabled)(void *ctx, bool enabled);
    esp_err_t (*set_input_current_limit)(void *ctx, uint16_t current_ma);
    esp_err_t (*set_charge_current)(void *ctx, uint16_t current_ma);
    esp_err_t (*set_charge_voltage)(void *ctx, uint16_t voltage_mv);
} solar_os_charger_ops_t;

typedef struct {
    const char *name;
    const char *driver;
    solar_os_charger_range_t input_current_limit_ma;
    solar_os_charger_range_t charge_current_ma;
    solar_os_charger_range_t charge_voltage_mv;
    const solar_os_charger_ops_t *ops;
    void *ctx;
} solar_os_charger_registration_t;

typedef struct {
    char name[SOLAR_OS_CHARGER_NAME_MAX];
    char driver[SOLAR_OS_CHARGER_DRIVER_MAX];
    solar_os_charger_range_t input_current_limit_ma;
    solar_os_charger_range_t charge_current_ma;
    solar_os_charger_range_t charge_voltage_mv;
} solar_os_charger_info_t;

esp_err_t solar_os_charger_register(
    const solar_os_charger_registration_t *registration);
esp_err_t solar_os_charger_unregister(const char *name);
size_t solar_os_charger_count(void);
bool solar_os_charger_get(size_t index, solar_os_charger_info_t *info);
esp_err_t solar_os_charger_read_status(const char *name,
                                       solar_os_charger_status_t *status);
esp_err_t solar_os_charger_set_enabled(const char *name, bool enabled);
esp_err_t solar_os_charger_set_input_current_limit(const char *name,
                                                   uint16_t current_ma);
esp_err_t solar_os_charger_set_charge_current(const char *name,
                                              uint16_t current_ma);
esp_err_t solar_os_charger_set_charge_voltage(const char *name,
                                              uint16_t voltage_mv);
const char *solar_os_charger_state_name(solar_os_charger_state_t state);
