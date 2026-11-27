#include "solar_os_battery.h"
#include "solar_os_board.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "solar_os_battery_trend.h"
#include "solar_os_log.h"
#include "solar_os_stream.h"

#include "nvs.h"

#define BATTERY_NVS_NAMESPACE "battery"
#define BATTERY_NVS_CAPACITY_KEY "capacity"
#define BATTERY_NVS_MIN_MV_KEY "min_mv"
#define BATTERY_NVS_MAX_MV_KEY "max_mv"
#define BATTERY_NVS_RUNTIME_KEY "runtime_min"

#define BATTERY_DEFAULT_MIN_MV 3000U
#define BATTERY_DEFAULT_MAX_MV 4120U
#define BATTERY_ALLOWED_MIN_MV 2500U
#define BATTERY_ALLOWED_MAX_MV 5000U
#define BATTERY_CAPACITY_MAX_MAH 100000U
#define BATTERY_VOLTAGE_AVG_WINDOW 8U
#define BATTERY_CHARGE_STEP_MV 20
#define BATTERY_CHARGE_STEP_MAX_MS (2U * 60U * 1000U)

#ifndef SOLAR_OS_BOARD_DEFAULT_BATTERY_CAPACITY_MAH
#define SOLAR_OS_BOARD_DEFAULT_BATTERY_CAPACITY_MAH 0U
#endif

static solar_os_battery_config_t battery_config = {
    .capacity_mah = SOLAR_OS_BOARD_DEFAULT_BATTERY_CAPACITY_MAH,
    .min_voltage_mv = BATTERY_DEFAULT_MIN_MV,
    .max_voltage_mv = BATTERY_DEFAULT_MAX_MV,
};
static bool battery_config_loaded;
static solar_os_battery_provider_t battery_provider;
static char battery_provider_owner[24];
static bool battery_initialized;
static bool battery_stream_registered;
static bool battery_provider_changing;
static unsigned battery_provider_reads;
static portMUX_TYPE battery_provider_lock = portMUX_INITIALIZER_UNLOCKED;

static solar_os_battery_monitor_status_t monitor_status = {
    .trend = SOLAR_OS_BATTERY_TREND_UNKNOWN,
    .last_error = ESP_OK,
};
static uint16_t battery_voltage_samples[BATTERY_VOLTAGE_AVG_WINDOW];
static size_t battery_voltage_sample_count;
static size_t battery_voltage_sample_index;
static uint32_t battery_voltage_sample_sum;
static bool battery_voltage_external_power_valid;
static bool battery_voltage_external_power;
static portMUX_TYPE battery_voltage_lock = portMUX_INITIALIZER_UNLOCKED;
static solar_os_battery_trend_estimator_t monitor_trend;
static struct {
    bool sample_valid;
    bool charging;
    uint16_t voltage_mv;
    uint32_t tick_ms;
} monitor_charge;

static esp_err_t battery_stream_read_scalar(
    void *user,
    const solar_os_stream_read_options_t *options,
    float *value)
{
    (void)user;
    (void)options;
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_battery_status_t status;
    const esp_err_t err = solar_os_battery_get_status(&status);
    if (err == ESP_OK) {
        *value = (float)status.voltage_mv / 1000.0f;
    }
    return err;
}

static esp_err_t battery_register_stream(void)
{
    solar_os_stream_driver_t driver = {
        .info = {
            .type = SOLAR_OS_STREAM_TYPE_SCALAR,
            .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
            .sharing = SOLAR_OS_STREAM_SHARING_SHARED,
        },
        .read_scalar = battery_stream_read_scalar,
        .user = &battery_provider,
    };
    strlcpy(driver.info.id, "battery", sizeof(driver.info.id));
    strlcpy(driver.info.provider, "battery", sizeof(driver.info.provider));
    strlcpy(driver.info.device, "battery0", sizeof(driver.info.device));
    strlcpy(driver.info.unit, "V", sizeof(driver.info.unit));
    strlcpy(driver.info.format, "f32", sizeof(driver.info.format));
    strlcpy(driver.info.summary, "battery voltage", sizeof(driver.info.summary));
    return solar_os_stream_register(&driver);
}

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

    uint32_t runtime_min = 0U;
    if (nvs_get_u32(nvs, BATTERY_NVS_RUNTIME_KEY, &runtime_min) == ESP_OK &&
        runtime_min <= SOLAR_OS_BATTERY_RUNTIME_MAX_MIN) {
        battery_config.runtime_min = runtime_min;
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
        ret = nvs_set_u32(nvs, BATTERY_NVS_RUNTIME_KEY, battery_config.runtime_min);
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

static void battery_reset_voltage_average(void)
{
    battery_voltage_sample_count = 0U;
    battery_voltage_sample_index = 0U;
    battery_voltage_sample_sum = 0U;
}

static uint16_t battery_push_voltage_average(uint16_t mv, bool power_valid,
                                             bool external_power, bool reset_average)
{
    portENTER_CRITICAL(&battery_voltage_lock);
    if (reset_average || (battery_voltage_sample_count > 0U &&
        (power_valid != battery_voltage_external_power_valid ||
         (power_valid && external_power != battery_voltage_external_power)))) {
        battery_reset_voltage_average();
    }
    battery_voltage_external_power_valid = power_valid;
    battery_voltage_external_power = external_power;
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

    const uint16_t averaged = (uint16_t)(
        (battery_voltage_sample_sum + (battery_voltage_sample_count / 2U)) /
        battery_voltage_sample_count);
    portEXIT_CRITICAL(&battery_voltage_lock);
    return averaged;
}

static bool battery_monitor_indicates_external_power(void)
{
    return monitor_status.running && monitor_charge.charging;
}

static bool battery_provider_snapshot(solar_os_battery_provider_t *provider)
{
    if (provider == NULL) {
        return false;
    }
    portENTER_CRITICAL(&battery_provider_lock);
    const bool available = battery_provider_owner[0] != '\0' && !battery_provider_changing;
    if (available) {
        *provider = battery_provider;
        battery_provider_reads++;
    }
    portEXIT_CRITICAL(&battery_provider_lock);
    return available;
}

static void battery_provider_read_done(void)
{
    portENTER_CRITICAL(&battery_provider_lock);
    battery_provider_reads--;
    portEXIT_CRITICAL(&battery_provider_lock);
}

static void battery_monitor_reset_estimate(uint32_t interval_ms)
{
    monitor_status.sample_count = 0;
    monitor_status.last_sample_ms = 0;
    monitor_status.last_voltage_mv = 0;
    monitor_status.last_percent = 0;
    monitor_status.adc_calibrated = false;
    monitor_status.trend = SOLAR_OS_BATTERY_TREND_UNKNOWN;
    monitor_status.external_power = false;
    monitor_status.external_power_known = false;
    monitor_status.slope_mvh = 0;
    monitor_status.time_left_min = 0;
    monitor_status.time_left_valid = false;
    monitor_status.time_left_seeded = false;
    monitor_status.full_runtime_min = 0U;
    memset(&monitor_status.short_window, 0, sizeof(monitor_status.short_window));
    memset(&monitor_status.long_window, 0, sizeof(monitor_status.long_window));
    monitor_status.last_error = ESP_OK;
    memset(&monitor_charge, 0, sizeof(monitor_charge));
    solar_os_battery_trend_init(&monitor_trend, interval_ms);
}

static bool battery_monitor_fast_charge(uint32_t now_ms, uint16_t mv,
                                        const solar_os_battery_status_t *status)
{
    /* A known absent supply always wins over voltage recovery under load.
     * Charging telemetry is applied separately by battery_get_status(). */
    if (status->external_power_known && !status->external_power) {
        memset(&monitor_charge, 0, sizeof(monitor_charge));
        return false;
    }
    const bool previous = monitor_charge.charging;
    const uint32_t elapsed = now_ms - monitor_charge.tick_ms;
    if (monitor_charge.sample_valid && elapsed > 0U && elapsed <= BATTERY_CHARGE_STEP_MAX_MS) {
        const int delta = (int)mv - monitor_charge.voltage_mv;
        if (delta >= BATTERY_CHARGE_STEP_MV) monitor_charge.charging = true;
        else if (delta <= -BATTERY_CHARGE_STEP_MV) monitor_charge.charging = false;
    }
    monitor_charge.sample_valid = true;
    monitor_charge.voltage_mv = mv;
    monitor_charge.tick_ms = now_ms;
    return previous != monitor_charge.charging;
}

static void battery_monitor_window_status(
    solar_os_battery_window_status_t *window,
    const solar_os_battery_trend_result_t *estimate,
    bool valid)
{
    memset(window, 0, sizeof(*window));
    if (valid) {
        window->valid = true;
        window->span_ms = estimate->span_ms;
        window->voltage_drop_mvh = -estimate->slope_mvh;
    }
}

static void battery_monitor_window_current(solar_os_battery_window_status_t *window)
{
    window->current_valid = false;
    window->current_ma = 0;
    if (!window->valid || battery_config.capacity_mah == 0U) {
        return;
    }
    /* This is the same linear voltage/capacity model as the voltage-based
     * charge estimate, not a current-sensor measurement. Positive is drain. */
    const int64_t current_ma = (int64_t)battery_config.capacity_mah *
        window->voltage_drop_mvh /
        (battery_config.max_voltage_mv - battery_config.min_voltage_mv);
    if (current_ma >= INT32_MIN && current_ma <= INT32_MAX) {
        window->current_ma = (int32_t)current_ma;
        window->current_valid = true;
    }
}

static void battery_monitor_refresh_estimates(solar_os_battery_monitor_status_t *monitor)
{
    battery_load_config();
    battery_monitor_window_current(&monitor->short_window);
    battery_monitor_window_current(&monitor->long_window);
    monitor->time_left_min = 0U;
    monitor->time_left_valid = false;
    monitor->time_left_seeded = false;
    monitor->full_runtime_min = 0U;
    if (monitor->sample_count == 0U || monitor->external_power ||
        (!monitor->external_power_known &&
         monitor->trend != SOLAR_OS_BATTERY_TREND_DISCHARGING)) {
        return;
    }

    const uint32_t remaining_mv = monitor->last_voltage_mv > battery_config.min_voltage_mv ?
        (uint32_t)(monitor->last_voltage_mv - battery_config.min_voltage_mv) : 0U;
    const uint32_t range_mv = battery_config.max_voltage_mv - battery_config.min_voltage_mv;
    if (monitor->trend == SOLAR_OS_BATTERY_TREND_DISCHARGING && monitor->slope_mvh < 0) {
        monitor->time_left_min = (remaining_mv * 60U) / (uint32_t)(-monitor->slope_mvh);
        monitor->full_runtime_min = (range_mv * 60U) / (uint32_t)(-monitor->slope_mvh);
        monitor->time_left_valid = true;
    } else if (battery_config.runtime_min > 0U) {
        const uint32_t available_mv = remaining_mv < range_mv ? remaining_mv : range_mv;
        monitor->time_left_min = (uint32_t)(
            ((uint64_t)battery_config.runtime_min * available_mv) / range_mv);
        monitor->full_runtime_min = battery_config.runtime_min;
        monitor->time_left_valid = true;
        monitor->time_left_seeded = true;
    }
}

static void battery_monitor_store_runtime(const solar_os_battery_trend_result_t *estimate)
{
    const uint32_t range_mv = battery_config.max_voltage_mv - battery_config.min_voltage_mv;
    const uint32_t runtime_min = (range_mv * 60U) / (uint32_t)(-estimate->slope_mvh);
    if (runtime_min == 0U || runtime_min > SOLAR_OS_BATTERY_RUNTIME_MAX_MIN ||
        runtime_min == battery_config.runtime_min) {
        return;
    }

    const uint32_t previous_runtime = battery_config.runtime_min;
    const esp_err_t err = solar_os_battery_set_runtime_min(runtime_min);
    if (err != ESP_OK) {
        /* Keep the last saved seed, and retry on the next new estimate. */
        battery_config.runtime_min = previous_runtime;
        SOLAR_OS_LOGW("solar_os_battery", "saving learned runtime failed: 0x%x", (unsigned)err);
    }
}

static void battery_monitor_update_estimate(const solar_os_battery_status_t *status,
                                            bool long_window_updated)
{
    monitor_status.trend = SOLAR_OS_BATTERY_TREND_UNKNOWN;
    monitor_status.external_power_known = status->external_power_known;
    monitor_status.slope_mvh = 0;
    solar_os_battery_trend_result_t estimate;
    const bool short_valid = solar_os_battery_trend_estimate(&monitor_trend, &estimate);
    solar_os_battery_trend_result_t long_estimate;
    bool long_valid = solar_os_battery_trend_estimate_long(&monitor_trend, &long_estimate);
    battery_monitor_window_status(&monitor_status.short_window, &estimate, short_valid);
    battery_monitor_window_status(&monitor_status.long_window, &long_estimate, long_valid);

    const bool was_charging = monitor_charge.charging;
    if (short_valid && monitor_charge.sample_valid) {
        /* Once detected, even a weak positive fitted slope keeps charging
         * latched; it need not pass the initial rise thresholds again. */
        monitor_charge.charging = estimate.trend == SOLAR_OS_BATTERY_VOLTAGE_TREND_RISING ||
            (was_charging && estimate.slope_mvh > 0);
    }
    monitor_status.external_power = status->external_power_known ?
        status->external_power : monitor_charge.charging;
    if (!status->external_power_known && (was_charging || monitor_charge.charging)) {
        /* Inferred charging must not become a learned discharge rate, even
         * while the rise is too small to classify as a strong short trend. */
        solar_os_battery_trend_reset_long(&monitor_trend);
        memset(&monitor_status.long_window, 0, sizeof(monitor_status.long_window));
        long_valid = false;
    }

    if (!short_valid) {
        battery_monitor_refresh_estimates(&monitor_status);
        return;
    }
    monitor_status.slope_mvh = estimate.slope_mvh;

    if (!monitor_status.external_power) {
        /* Known battery power lets the long discharge rate survive short
         * voltage rebounds. Voltage-only providers still honor a recent rise. */
        if ((estimate.trend != SOLAR_OS_BATTERY_VOLTAGE_TREND_RISING ||
             status->external_power_known) &&
            long_valid &&
            long_estimate.trend == SOLAR_OS_BATTERY_VOLTAGE_TREND_FALLING) {
            estimate = long_estimate;
            monitor_status.slope_mvh = estimate.slope_mvh;
            if (long_window_updated) {
                battery_monitor_store_runtime(&long_estimate);
            }
        }
    }

    if (estimate.trend == SOLAR_OS_BATTERY_VOLTAGE_TREND_RISING) {
        monitor_status.trend = SOLAR_OS_BATTERY_TREND_CHARGING;
        battery_monitor_refresh_estimates(&monitor_status);
        return;
    }
    if (estimate.trend == SOLAR_OS_BATTERY_VOLTAGE_TREND_FALLING) {
        monitor_status.trend = SOLAR_OS_BATTERY_TREND_DISCHARGING;
        battery_monitor_refresh_estimates(&monitor_status);
        return;
    }

    monitor_status.trend = SOLAR_OS_BATTERY_TREND_FLAT;
    battery_monitor_refresh_estimates(&monitor_status);
}

static esp_err_t battery_publish_provider(bool rollback)
{
    const esp_err_t error = battery_register_stream();
    portENTER_CRITICAL(&battery_provider_lock);
    battery_stream_registered = error == ESP_OK;
    if (error != ESP_OK && rollback) {
        battery_provider = (solar_os_battery_provider_t){0};
        battery_provider_owner[0] = '\0';
    }
    battery_provider_changing = false;
    portEXIT_CRITICAL(&battery_provider_lock);
    return error;
}

esp_err_t solar_os_battery_init(void)
{
    battery_load_config();
    portENTER_CRITICAL(&battery_provider_lock);
    battery_initialized = true;
    if (!battery_provider_owner[0] || battery_provider_changing || battery_stream_registered) {
        const esp_err_t error = !battery_provider_owner[0] ? ESP_ERR_NOT_SUPPORTED :
            battery_provider_changing ? ESP_ERR_INVALID_STATE : ESP_OK;
        portEXIT_CRITICAL(&battery_provider_lock);
        return error;
    }
    battery_provider_changing = true;
    portEXIT_CRITICAL(&battery_provider_lock);
    return battery_publish_provider(false);
}

esp_err_t solar_os_battery_register_provider(const char *owner, const solar_os_battery_provider_t *provider)
{
    if (!owner || !*owner ||
        strnlen(owner, sizeof(battery_provider_owner)) >= sizeof(battery_provider_owner) ||
        !provider || !provider->read) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&battery_provider_lock);
    if (battery_provider_owner[0]) {
        portEXIT_CRITICAL(&battery_provider_lock);
        return ESP_ERR_INVALID_STATE;
    }
    battery_provider = *provider;
    strlcpy(battery_provider_owner, owner, sizeof(battery_provider_owner));
    const bool publish = battery_initialized;
    battery_provider_changing = publish;
    portEXIT_CRITICAL(&battery_provider_lock);
    return publish ? battery_publish_provider(true) : ESP_OK;
}

static esp_err_t battery_unregister_provider(const char *owner, const void *user)
{
    if (!owner || !*owner) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&battery_provider_lock);
    if (strcmp(battery_provider_owner, owner) || (user && battery_provider.user != user)) {
        portEXIT_CRITICAL(&battery_provider_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (battery_provider_changing || battery_provider_reads) {
        portEXIT_CRITICAL(&battery_provider_lock);
        return ESP_ERR_INVALID_STATE;
    }
    battery_provider_changing = true;
    const bool published = battery_stream_registered;
    portEXIT_CRITICAL(&battery_provider_lock);

    esp_err_t error = ESP_OK;
    if (published) {
        const char *id = "battery";
        const void *owner_token = &battery_provider;
        error = solar_os_stream_unregister_many_owned(&id, &owner_token, 1);
    }
    if (error == ESP_OK) solar_os_battery_monitor_stop();

    portENTER_CRITICAL(&battery_provider_lock);
    if (error == ESP_OK) {
        battery_provider = (solar_os_battery_provider_t){0};
        battery_provider_owner[0] = '\0';
        battery_stream_registered = false;
    }
    battery_provider_changing = false;
    portEXIT_CRITICAL(&battery_provider_lock);
    return error;
}

esp_err_t solar_os_battery_unregister_provider(const char *owner)
{
    return battery_unregister_provider(owner, NULL);
}

esp_err_t solar_os_battery_unregister_provider_owned(const char *owner, const void *user)
{
    return user ? battery_unregister_provider(owner, user) : ESP_ERR_INVALID_ARG;
}

bool solar_os_battery_has_provider(void)
{
    portENTER_CRITICAL(&battery_provider_lock);
    const bool available = battery_provider_owner[0] != '\0' && !battery_provider_changing;
    portEXIT_CRITICAL(&battery_provider_lock);
    return available;
}

static esp_err_t battery_get_status(solar_os_battery_status_t *status, uint16_t *raw_mv)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    battery_load_config();

    solar_os_battery_provider_t provider;
    if (!battery_provider_snapshot(&provider)) {
        memset(status, 0, sizeof(*status));
        return ESP_ERR_NOT_SUPPORTED;
    }
    solar_os_battery_sample_t sample = {0};
    const esp_err_t ret = provider.read(provider.user, &sample);
    if (ret != ESP_OK) {
        battery_provider_read_done();
        return ret;
    }

    if (raw_mv != NULL) *raw_mv = sample.battery_mv;
    const uint16_t averaged_mv = battery_push_voltage_average(sample.battery_mv,
        sample.external_power_valid, sample.external_power, false);
    status->voltage_mv = averaged_mv;
    status->percent = sample.percent_valid
        ? (sample.percent > 100U ? 100U : sample.percent)
        : battery_percent_from_voltage(averaged_mv);
    status->percent_estimated = !sample.percent_valid;
    status->adc_calibrated = sample.calibrated;
    const bool inferred_charging = battery_monitor_indicates_external_power() &&
        (!sample.external_power_valid || sample.external_power);
    status->external_power = sample.external_power_valid
        ? sample.external_power
        : inferred_charging;
    status->charging = sample.charging_valid
        ? sample.charging
        : inferred_charging;
    status->charging_known = sample.charging_valid;
    status->external_power_known = sample.external_power_valid;
    battery_provider_read_done();
    return ESP_OK;
}

esp_err_t solar_os_battery_get_status(solar_os_battery_status_t *status)
{
    return battery_get_status(status, NULL);
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

esp_err_t solar_os_battery_set_runtime_min(uint32_t runtime_min)
{
    battery_load_config();
    if (runtime_min > SOLAR_OS_BATTERY_RUNTIME_MAX_MIN) {
        return ESP_ERR_INVALID_ARG;
    }
    battery_config.runtime_min = runtime_min;
    return battery_save_config();
}

esp_err_t solar_os_battery_monitor_start(uint32_t interval_ms)
{
    if (interval_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_battery_has_provider()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    battery_load_config();
    monitor_status.interval_ms = interval_ms;
    battery_monitor_reset_estimate(interval_ms);
    monitor_status.running = true;
    return ESP_OK;
}

void solar_os_battery_monitor_stop(void)
{
    monitor_status.running = false;
}

esp_err_t solar_os_battery_monitor_sample(uint32_t now_ms)
{
    solar_os_battery_status_t status;
    uint16_t raw_mv;
    const esp_err_t ret = battery_get_status(&status, &raw_mv);
    monitor_status.last_error = ret;
    if (ret != ESP_OK) {
        return ret;
    }

    if (monitor_status.sample_count > 0U &&
        (status.external_power_known != monitor_status.external_power_known ||
         (status.external_power_known &&
          status.external_power != monitor_status.external_power))) {
        /* Switching supply can cause an immediate voltage step. Neither
         * regression may interpret that step as sustained drain or charge. */
        solar_os_battery_trend_init(&monitor_trend, monitor_status.interval_ms);
        memset(&monitor_charge, 0, sizeof(monitor_charge));
    }
    if (battery_monitor_fast_charge(now_ms, raw_mv, &status)) {
        /* Detect the supply step before averaging hides it. Start both rate
         * windows after the step so it cannot produce a spurious current/ETA. */
        solar_os_battery_trend_init(&monitor_trend, monitor_status.interval_ms);
        status.voltage_mv = battery_push_voltage_average(raw_mv,
            status.external_power_known, status.external_power, true);
        if (status.percent_estimated) status.percent = battery_percent_from_voltage(status.voltage_mv);
    }
    monitor_status.sample_count++;
    monitor_status.last_sample_ms = now_ms;
    monitor_status.last_voltage_mv = status.voltage_mv;
    monitor_status.last_percent = status.percent;
    monitor_status.adc_calibrated = status.adc_calibrated;
    const uint32_t previous_bucket_start = monitor_trend.bucket_start_ms;
    solar_os_battery_trend_push(&monitor_trend, now_ms, status.voltage_mv);
    const bool long_window_updated = monitor_trend.long_sample_count > 0U &&
        monitor_trend.bucket_start_ms != previous_bucket_start;
    battery_monitor_update_estimate(&status, long_window_updated);
    return ESP_OK;
}

void solar_os_battery_monitor_get_status(solar_os_battery_monitor_status_t *status)
{
    if (status == NULL) {
        return;
    }

    *status = monitor_status;
    battery_monitor_refresh_estimates(status);
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
