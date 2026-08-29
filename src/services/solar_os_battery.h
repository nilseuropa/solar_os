#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    SOLAR_OS_BATTERY_TREND_UNKNOWN,
    SOLAR_OS_BATTERY_TREND_DISCHARGING,
    SOLAR_OS_BATTERY_TREND_FLAT,
    SOLAR_OS_BATTERY_TREND_CHARGING,
} solar_os_battery_trend_t;

typedef struct {
    uint16_t voltage_mv;
    uint8_t percent;
    bool percent_estimated;
    bool adc_calibrated;
    bool external_power;
} solar_os_battery_status_t;

typedef struct {
    uint32_t capacity_mah;
    uint16_t min_voltage_mv;
    uint16_t max_voltage_mv;
} solar_os_battery_config_t;

typedef struct {
    bool running;
    uint32_t interval_ms;
    uint32_t sample_count;
    uint32_t last_sample_ms;
    uint16_t last_voltage_mv;
    uint8_t last_percent;
    bool adc_calibrated;
    solar_os_battery_trend_t trend;
    bool external_power;
    int32_t slope_mvh;
    uint32_t time_left_min;
    bool time_left_valid;
    esp_err_t last_error;
} solar_os_battery_monitor_status_t;

typedef struct {
    uint16_t battery_mv;
    bool calibrated;
} solar_os_battery_sample_t;

typedef esp_err_t (*solar_os_battery_provider_read_fn_t)(
    void *user,
    solar_os_battery_sample_t *sample);

typedef struct {
    solar_os_battery_provider_read_fn_t read;
    void *user;
} solar_os_battery_provider_t;

esp_err_t solar_os_battery_init(void);
esp_err_t solar_os_battery_register_provider(
    const char *owner,
    const solar_os_battery_provider_t *provider);
esp_err_t solar_os_battery_unregister_provider(const char *owner);
bool solar_os_battery_has_provider(void);
esp_err_t solar_os_battery_get_status(solar_os_battery_status_t *status);
void solar_os_battery_get_config(solar_os_battery_config_t *config);
esp_err_t solar_os_battery_set_capacity_mah(uint32_t capacity_mah);
esp_err_t solar_os_battery_set_min_voltage_mv(uint16_t min_voltage_mv);
esp_err_t solar_os_battery_set_max_voltage_mv(uint16_t max_voltage_mv);
esp_err_t solar_os_battery_monitor_start(uint32_t interval_ms);
void solar_os_battery_monitor_stop(void);
esp_err_t solar_os_battery_monitor_sample(uint32_t now_ms);
void solar_os_battery_monitor_get_status(solar_os_battery_monitor_status_t *status);
const char *solar_os_battery_trend_name(solar_os_battery_trend_t trend);
