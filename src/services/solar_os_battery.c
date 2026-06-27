#include "solar_os_battery.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "solar_os_board_caps.h"

#if SOLAR_OS_BOARD_HAS_BATTERY
#include "solar_os_board_battery.h"
#endif

#include "nvs.h"

#define BATTERY_NVS_NAMESPACE "battery"
#define BATTERY_NVS_CAPACITY_KEY "capacity"
#define BATTERY_NVS_MIN_MV_KEY "min_mv"
#define BATTERY_NVS_MAX_MV_KEY "max_mv"

#define BATTERY_DEFAULT_MIN_MV 3000U
#define BATTERY_DEFAULT_MAX_MV 4120U
#define BATTERY_ALLOWED_MIN_MV 2500U
#define BATTERY_ALLOWED_MAX_MV 5000U
#define BATTERY_CAPACITY_MAX_MAH 100000U
#define BATTERY_VOLTAGE_AVG_WINDOW 8U
#define BATTERY_MONITOR_HISTORY 16U
#define BATTERY_TREND_WINDOW 8U
#define BATTERY_TREND_MIN_SAMPLES 4U
#define BATTERY_TREND_THRESHOLD_MVH 10

typedef struct {
    uint32_t tick_ms;
    uint16_t voltage_mv;
} battery_monitor_sample_t;

static solar_os_battery_config_t battery_config = {
    .capacity_mah = 0,
    .min_voltage_mv = BATTERY_DEFAULT_MIN_MV,
    .max_voltage_mv = BATTERY_DEFAULT_MAX_MV,
};
static bool battery_config_loaded;

static solar_os_battery_monitor_status_t monitor_status = {
    .trend = SOLAR_OS_BATTERY_TREND_UNKNOWN,
    .last_error = ESP_OK,
};
static uint16_t battery_voltage_samples[BATTERY_VOLTAGE_AVG_WINDOW];
static size_t battery_voltage_sample_count;
static size_t battery_voltage_sample_index;
static uint32_t battery_voltage_sample_sum;
static battery_monitor_sample_t monitor_samples[BATTERY_MONITOR_HISTORY];
static size_t monitor_sample_count;

static bool battery_voltage_range_valid(uint16_t min_mv, uint16_t max_mv)
{
    return min_mv >= BATTERY_ALLOWED_MIN_MV &&
        max_mv <= BATTERY_ALLOWED_MAX_MV &&
        min_mv < max_mv;
}

static void battery_load_config(void)
{
    if (battery_config_loaded) {
        return;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BATTERY_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        battery_config_loaded = true;
        return;
    }
    if (ret != ESP_OK) {
        battery_config_loaded = true;
        return;
    }

    uint32_t capacity = 0;
    if (nvs_get_u32(nvs, BATTERY_NVS_CAPACITY_KEY, &capacity) == ESP_OK &&
        capacity <= BATTERY_CAPACITY_MAX_MAH) {
        battery_config.capacity_mah = capacity;
    }

    uint16_t min_mv = battery_config.min_voltage_mv;
    uint16_t max_mv = battery_config.max_voltage_mv;
    (void)nvs_get_u16(nvs, BATTERY_NVS_MIN_MV_KEY, &min_mv);
    (void)nvs_get_u16(nvs, BATTERY_NVS_MAX_MV_KEY, &max_mv);
    if (battery_voltage_range_valid(min_mv, max_mv)) {
        battery_config.min_voltage_mv = min_mv;
        battery_config.max_voltage_mv = max_mv;
    }

    nvs_close(nvs);
    battery_config_loaded = true;
}

static esp_err_t battery_save_config(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BATTERY_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u32(nvs, BATTERY_NVS_CAPACITY_KEY, battery_config.capacity_mah);
    if (ret == ESP_OK) {
        ret = nvs_set_u16(nvs, BATTERY_NVS_MIN_MV_KEY, battery_config.min_voltage_mv);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u16(nvs, BATTERY_NVS_MAX_MV_KEY, battery_config.max_voltage_mv);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static uint8_t battery_percent_from_voltage(uint16_t mv)
{
    battery_load_config();
    const uint16_t min_mv = battery_config.min_voltage_mv;
    const uint16_t max_mv = battery_config.max_voltage_mv;

    if (mv <= min_mv) {
        return 0;
    }
    if (mv >= max_mv) {
        return 100;
    }

    return (uint8_t)(((uint32_t)(mv - min_mv) * 100U) / (uint32_t)(max_mv - min_mv));
}

static bool battery_voltage_external_power(uint16_t mv)
{
    battery_load_config();
    return mv > battery_config.max_voltage_mv;
}

static uint16_t battery_push_voltage_average(uint16_t mv)
{
    if (battery_voltage_sample_count < BATTERY_VOLTAGE_AVG_WINDOW) {
        battery_voltage_samples[battery_voltage_sample_count++] = mv;
        battery_voltage_sample_sum += mv;
    } else {
        battery_voltage_sample_sum -= battery_voltage_samples[battery_voltage_sample_index];
        battery_voltage_samples[battery_voltage_sample_index] = mv;
        battery_voltage_sample_sum += mv;
        battery_voltage_sample_index++;
        if (battery_voltage_sample_index >= BATTERY_VOLTAGE_AVG_WINDOW) {
            battery_voltage_sample_index = 0;
        }
    }

    return (uint16_t)((battery_voltage_sample_sum + (battery_voltage_sample_count / 2U)) /
                      battery_voltage_sample_count);
}

static bool battery_monitor_indicates_external_power(void)
{
    return monitor_status.running &&
        monitor_status.sample_count >= BATTERY_TREND_MIN_SAMPLES &&
        monitor_status.trend == SOLAR_OS_BATTERY_TREND_CHARGING &&
        monitor_status.external_power;
}

static void battery_monitor_reset_estimate(void)
{
    monitor_status.sample_count = 0;
    monitor_status.last_sample_ms = 0;
    monitor_status.last_voltage_mv = 0;
    monitor_status.last_percent = 0;
    monitor_status.adc_calibrated = false;
    monitor_status.trend = SOLAR_OS_BATTERY_TREND_UNKNOWN;
    monitor_status.external_power = false;
    monitor_status.slope_mvh = 0;
    monitor_status.time_left_min = 0;
    monitor_status.time_left_valid = false;
    monitor_status.last_error = ESP_OK;
    monitor_sample_count = 0;
    memset(monitor_samples, 0, sizeof(monitor_samples));
}

static void battery_monitor_push_sample(uint32_t now_ms, uint16_t voltage_mv)
{
    if (monitor_sample_count < BATTERY_MONITOR_HISTORY) {
        monitor_samples[monitor_sample_count++] = (battery_monitor_sample_t){
            .tick_ms = now_ms,
            .voltage_mv = voltage_mv,
        };
        return;
    }

    memmove(&monitor_samples[0],
            &monitor_samples[1],
            sizeof(monitor_samples[0]) * (BATTERY_MONITOR_HISTORY - 1U));
    monitor_samples[BATTERY_MONITOR_HISTORY - 1U] = (battery_monitor_sample_t){
        .tick_ms = now_ms,
        .voltage_mv = voltage_mv,
    };
}

static bool battery_monitor_calculate_slope(int32_t *slope_mvh)
{
    if (slope_mvh == NULL || monitor_sample_count < BATTERY_TREND_MIN_SAMPLES) {
        return false;
    }

    const size_t start = monitor_sample_count > BATTERY_TREND_WINDOW ?
        monitor_sample_count - BATTERY_TREND_WINDOW :
        0;
    const size_t count = monitor_sample_count - start;
    if (count < BATTERY_TREND_MIN_SAMPLES) {
        return false;
    }

    const uint32_t base_ms = monitor_samples[start].tick_ms;
    int64_t sum_t = 0;
    int64_t sum_v = 0;
    int64_t sum_tt = 0;
    int64_t sum_tv = 0;

    for (size_t i = start; i < monitor_sample_count; i++) {
        const int64_t t = (int64_t)(monitor_samples[i].tick_ms - base_ms);
        const int64_t v = (int64_t)monitor_samples[i].voltage_mv;
        sum_t += t;
        sum_v += v;
        sum_tt += t * t;
        sum_tv += t * v;
    }

    const int64_t n = (int64_t)count;
    const int64_t denominator = (n * sum_tt) - (sum_t * sum_t);
    if (denominator == 0) {
        return false;
    }

    const int64_t numerator = (n * sum_tv) - (sum_t * sum_v);
    *slope_mvh = (int32_t)((numerator * 3600000LL) / denominator);
    return true;
}

static void battery_monitor_update_estimate(void)
{
    monitor_status.trend = SOLAR_OS_BATTERY_TREND_UNKNOWN;
    monitor_status.external_power = false;
    monitor_status.slope_mvh = 0;
    monitor_status.time_left_min = 0;
    monitor_status.time_left_valid = false;

    if (monitor_sample_count == 0) {
        return;
    }

    const battery_monitor_sample_t *newest = &monitor_samples[monitor_sample_count - 1U];
    if (battery_voltage_external_power(newest->voltage_mv)) {
        monitor_status.trend = SOLAR_OS_BATTERY_TREND_CHARGING;
        monitor_status.external_power = true;
        return;
    }

    int32_t slope_mvh = 0;
    if (!battery_monitor_calculate_slope(&slope_mvh)) {
        return;
    }
    monitor_status.slope_mvh = slope_mvh;

    if (slope_mvh > BATTERY_TREND_THRESHOLD_MVH) {
        monitor_status.trend = SOLAR_OS_BATTERY_TREND_CHARGING;
        monitor_status.external_power = true;
        return;
    }
    if (slope_mvh < -BATTERY_TREND_THRESHOLD_MVH) {
        monitor_status.trend = SOLAR_OS_BATTERY_TREND_DISCHARGING;
        battery_load_config();
        if (newest->voltage_mv <= battery_config.min_voltage_mv) {
            monitor_status.time_left_min = 0;
            monitor_status.time_left_valid = true;
            return;
        }

        const uint32_t remaining_mv = newest->voltage_mv - battery_config.min_voltage_mv;
        const uint32_t drain_mvh = (uint32_t)(-slope_mvh);
        if (drain_mvh > 0) {
            monitor_status.time_left_min = (remaining_mv * 60U) / drain_mvh;
            monitor_status.time_left_valid = true;
        }
        return;
    }

    monitor_status.trend = SOLAR_OS_BATTERY_TREND_FLAT;
}

esp_err_t solar_os_battery_init(void)
{
    battery_load_config();
#if !SOLAR_OS_BOARD_HAS_BATTERY
    return ESP_ERR_NOT_SUPPORTED;
#else
    return solar_os_board_battery_init();
#endif
}

esp_err_t solar_os_battery_get_status(solar_os_battery_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    battery_load_config();

#if !SOLAR_OS_BOARD_HAS_BATTERY
    memset(status, 0, sizeof(*status));
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_board_battery_sample_t sample;
    const esp_err_t ret = solar_os_board_battery_read(&sample);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint16_t averaged_mv = battery_push_voltage_average(sample.battery_mv);
    status->voltage_mv = averaged_mv;
    status->percent = battery_percent_from_voltage(averaged_mv);
    status->percent_estimated = true;
    status->adc_calibrated = sample.calibrated;
    status->external_power = battery_voltage_external_power(sample.battery_mv) ||
        battery_monitor_indicates_external_power();
    return ESP_OK;
#endif
}

void solar_os_battery_get_config(solar_os_battery_config_t *config)
{
    if (config == NULL) {
        return;
    }

    battery_load_config();
    *config = battery_config;
}

esp_err_t solar_os_battery_set_capacity_mah(uint32_t capacity_mah)
{
    battery_load_config();
    if (capacity_mah > BATTERY_CAPACITY_MAX_MAH) {
        return ESP_ERR_INVALID_ARG;
    }

    battery_config.capacity_mah = capacity_mah;
    return battery_save_config();
}

esp_err_t solar_os_battery_set_min_voltage_mv(uint16_t min_voltage_mv)
{
    battery_load_config();
    if (!battery_voltage_range_valid(min_voltage_mv, battery_config.max_voltage_mv)) {
        return ESP_ERR_INVALID_ARG;
    }

    battery_config.min_voltage_mv = min_voltage_mv;
    return battery_save_config();
}

esp_err_t solar_os_battery_set_max_voltage_mv(uint16_t max_voltage_mv)
{
    battery_load_config();
    if (!battery_voltage_range_valid(battery_config.min_voltage_mv, max_voltage_mv)) {
        return ESP_ERR_INVALID_ARG;
    }

    battery_config.max_voltage_mv = max_voltage_mv;
    return battery_save_config();
}

esp_err_t solar_os_battery_monitor_start(uint32_t interval_ms)
{
    if (interval_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_BOARD_HAS_BATTERY
    return ESP_ERR_NOT_SUPPORTED;
#else

    battery_load_config();
    battery_monitor_reset_estimate();
    monitor_status.running = true;
    monitor_status.interval_ms = interval_ms;
    return ESP_OK;
#endif
}

void solar_os_battery_monitor_stop(void)
{
    monitor_status.running = false;
}

esp_err_t solar_os_battery_monitor_sample(uint32_t now_ms)
{
    solar_os_battery_status_t status;
    const esp_err_t ret = solar_os_battery_get_status(&status);
    monitor_status.last_error = ret;
    if (ret != ESP_OK) {
        return ret;
    }

    monitor_status.sample_count++;
    monitor_status.last_sample_ms = now_ms;
    monitor_status.last_voltage_mv = status.voltage_mv;
    monitor_status.last_percent = status.percent;
    monitor_status.adc_calibrated = status.adc_calibrated;
    battery_monitor_push_sample(now_ms, status.voltage_mv);
    battery_monitor_update_estimate();
    return ESP_OK;
}

void solar_os_battery_monitor_get_status(solar_os_battery_monitor_status_t *status)
{
    if (status == NULL) {
        return;
    }

    *status = monitor_status;
}

const char *solar_os_battery_trend_name(solar_os_battery_trend_t trend)
{
    switch (trend) {
    case SOLAR_OS_BATTERY_TREND_DISCHARGING:
        return "discharging";
    case SOLAR_OS_BATTERY_TREND_FLAT:
        return "flat";
    case SOLAR_OS_BATTERY_TREND_CHARGING:
        return "charging";
    case SOLAR_OS_BATTERY_TREND_UNKNOWN:
    default:
        return "unknown";
    }
}
