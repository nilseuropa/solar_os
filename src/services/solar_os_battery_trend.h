#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_BATTERY_TREND_HISTORY 32U
#define SOLAR_OS_BATTERY_TREND_LONG_HISTORY 64U

typedef enum {
    SOLAR_OS_BATTERY_VOLTAGE_TREND_UNKNOWN,
    SOLAR_OS_BATTERY_VOLTAGE_TREND_STABLE,
    SOLAR_OS_BATTERY_VOLTAGE_TREND_RISING,
    SOLAR_OS_BATTERY_VOLTAGE_TREND_FALLING,
} solar_os_battery_voltage_trend_t;

typedef struct {
    uint32_t tick_ms;
    uint16_t voltage_mv;
} solar_os_battery_trend_sample_t;

typedef struct {
    uint32_t interval_ms;
    solar_os_battery_trend_sample_t samples[SOLAR_OS_BATTERY_TREND_HISTORY];
    size_t sample_count;
    solar_os_battery_trend_sample_t long_samples[SOLAR_OS_BATTERY_TREND_LONG_HISTORY];
    size_t long_sample_count;
    uint32_t bucket_start_ms;
    uint32_t bucket_count;
    uint64_t bucket_sum_mv;
    uint64_t bucket_sum_ms;
} solar_os_battery_trend_estimator_t;

typedef struct {
    solar_os_battery_voltage_trend_t trend;
    int32_t slope_mvh;
    int32_t delta_mv;
    uint32_t span_ms;
    size_t sample_count;
} solar_os_battery_trend_result_t;

void solar_os_battery_trend_init(
    solar_os_battery_trend_estimator_t *estimator,
    uint32_t interval_ms);

void solar_os_battery_trend_push(
    solar_os_battery_trend_estimator_t *estimator,
    uint32_t tick_ms,
    uint16_t voltage_mv);

bool solar_os_battery_trend_estimate(
    const solar_os_battery_trend_estimator_t *estimator,
    solar_os_battery_trend_result_t *result);

/* Slow discharge uses up to sixteen hours of fifteen-minute averages. */
bool solar_os_battery_trend_estimate_long(
    const solar_os_battery_trend_estimator_t *estimator,
    solar_os_battery_trend_result_t *result);

void solar_os_battery_trend_reset_long(
    solar_os_battery_trend_estimator_t *estimator);
