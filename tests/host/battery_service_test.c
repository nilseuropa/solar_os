#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "solar_os_battery.h"
#include "solar_os_log.h"
#include "solar_os_stream.h"

static solar_os_battery_sample_t provider_sample;
static unsigned stream_register_count;
static unsigned stream_unregister_count;
static uint32_t stored_runtime_min = 1440U;
static unsigned runtime_write_count;
static unsigned save_warning_count;
static esp_err_t nvs_write_error;

esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *format, ...)
{
    assert(level == SOLAR_OS_LOG_LEVEL_WARN);
    assert(strcmp(tag, "solar_os_battery") == 0);
    (void)format;
    save_warning_count++;
    return ESP_OK;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t length = strlen(src);
    if (size > 0U) {
        const size_t copy = length < size - 1U ? length : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return length;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    (void)name;
    if (mode == NVS_READWRITE && nvs_write_error != ESP_OK) {
        return nvs_write_error;
    }
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *value)
{
    (void)handle;
    if (strcmp(key, "runtime_min") == 0) {
        *value = stored_runtime_min;
        return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    (void)handle;
    if (strcmp(key, "runtime_min") == 0) {
        stored_runtime_min = value;
        runtime_write_count++;
    }
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

esp_err_t solar_os_stream_register(const solar_os_stream_driver_t *driver)
{
    assert(driver != NULL);
    assert(strcmp(driver->info.id, "battery") == 0);
    stream_register_count++;
    return ESP_OK;
}

esp_err_t solar_os_stream_unregister(const char *id)
{
    assert(id != NULL);
    assert(strcmp(id, "battery") == 0);
    stream_unregister_count++;
    return ESP_OK;
}

static esp_err_t read_battery(void *user, solar_os_battery_sample_t *sample)
{
    assert(user == &provider_sample);
    assert(sample != NULL);
    *sample = provider_sample;
    return ESP_OK;
}

static solar_os_battery_status_t read_status(void)
{
    solar_os_battery_status_t status;
    assert(solar_os_battery_get_status(&status) == ESP_OK);
    return status;
}

static solar_os_battery_monitor_status_t sample_monitor(uint32_t now_ms)
{
    solar_os_battery_monitor_status_t status;
    assert(solar_os_battery_monitor_sample(now_ms) == ESP_OK);
    solar_os_battery_monitor_get_status(&status);
    return status;
}

static void test_monitor_power_transition_step(void)
{
    provider_sample = (solar_os_battery_sample_t) {
        .battery_mv = 3924U,
        .external_power_valid = true,
        .external_power = true,
        .charging_valid = true,
    };
    for (unsigned i = 0U; i < 8U; i++) {
        (void)read_status();
    }
    assert(solar_os_battery_monitor_start(10000U) == ESP_OK);
    for (uint32_t i = 0U; i < 18U; i++) {
        (void)sample_monitor(i * 10000U);
    }

    /* Zenith's trace: USB power disappeared at 186 s, with a one-sample
     * voltage step from 3924 to 3749 mV. It is not a discharge rate. */
    provider_sample.external_power = false;
    provider_sample.battery_mv = 3749U;
    solar_os_battery_monitor_status_t monitor = sample_monitor(180000U);
    assert(monitor.last_voltage_mv == 3749U);
    for (uint32_t i = 19U; i < 48U; i++) {
        monitor = sample_monitor(i * 10000U);
        assert(!monitor.short_window.valid && !monitor.long_window.valid);
        assert(!monitor.short_window.current_valid);
        assert(!monitor.time_left_valid);
    }
    monitor = sample_monitor(480000U);
    assert(monitor.sample_count == 49U);
    assert(monitor.short_window.valid && monitor.short_window.current_valid);
    assert(monitor.short_window.voltage_drop_mvh == 0);
    assert(monitor.short_window.current_ma == 0);
    assert(!monitor.time_left_valid);

    /* A configured starting estimate remains available while the new power
     * period has no established discharge rate. */
    assert(solar_os_battery_set_runtime_min(6000U) == ESP_OK);
    solar_os_battery_monitor_get_status(&monitor);
    assert(monitor.time_left_valid && monitor.time_left_seeded);

    provider_sample.external_power = true;
    provider_sample.battery_mv = 3924U;
    monitor = sample_monitor(490000U);
    assert(monitor.last_voltage_mv == 3924U);
    assert(!monitor.short_window.valid && !monitor.long_window.valid);
    assert(!monitor.time_left_valid);
    assert(solar_os_battery_set_runtime_min(0U) == ESP_OK);
    solar_os_battery_monitor_stop();
}

static void test_monitor_hardware_power(void)
{
    provider_sample = (solar_os_battery_sample_t) {
        .battery_mv = 4000U,
        .calibrated = true,
        .external_power_valid = true,
        .external_power = true,
        .charging_valid = true,
        .charging = false,
    };
    assert(solar_os_battery_monitor_start(10000U) == ESP_OK);
    solar_os_battery_monitor_status_t monitor = sample_monitor(0U);
    assert(monitor.external_power);
    assert(monitor.external_power_known);
    assert(monitor.trend == SOLAR_OS_BATTERY_TREND_UNKNOWN);
    assert(!monitor.time_left_valid);

    /* A suspended charger can have a falling voltage while USB is present. */
    for (uint32_t i = 1U; i <= 60U; i++) {
        provider_sample.battery_mv = (uint16_t)(4000U - i * 2U);
        monitor = sample_monitor(i * 10000U);
        assert(monitor.external_power);
        assert(!monitor.time_left_valid);
    }
    assert(monitor.trend == SOLAR_OS_BATTERY_TREND_DISCHARGING);
    assert(monitor.slope_mvh < 0);
    assert(!read_status().charging);

    provider_sample.external_power = false;
    monitor = sample_monitor(610000U);
    assert(!monitor.external_power);
    assert(monitor.external_power_known);
    assert(!monitor.time_left_valid);
    assert(!monitor.short_window.valid && !monitor.long_window.valid);
    assert(solar_os_battery_monitor_start(10000U) == ESP_OK);
    monitor = sample_monitor(0U);
    assert(monitor.external_power_known && !monitor.external_power);
    assert(monitor.trend == SOLAR_OS_BATTERY_TREND_UNKNOWN);

    /* Rising voltage must not override explicit absence of external power. */
    assert(solar_os_battery_monitor_start(10000U) == ESP_OK);
    for (uint32_t i = 0U; i <= 60U; i++) {
        provider_sample.battery_mv = (uint16_t)(3700U + i * 2U);
        monitor = sample_monitor(i * 10000U);
        assert(!monitor.external_power);
    }
    assert(monitor.trend == SOLAR_OS_BATTERY_TREND_CHARGING);
    provider_sample.charging_valid = false;
    assert(!read_status().charging);

    /* Voltage-only providers retain conservative rise/fall inference. */
    provider_sample.external_power_valid = false;
    monitor = sample_monitor(610000U);
    assert(!monitor.external_power);
    assert(!monitor.external_power_known);
    assert(monitor.trend == SOLAR_OS_BATTERY_TREND_UNKNOWN);
    for (uint32_t i = 62U; i <= 122U; i++) {
        provider_sample.battery_mv = (uint16_t)(3700U + i * 2U);
        monitor = sample_monitor(i * 10000U);
    }
    assert(monitor.external_power);
    assert(read_status().charging && !read_status().charging_known);
    assert(solar_os_battery_monitor_start(10000U) == ESP_OK);
    monitor = sample_monitor(0U);
    assert(!monitor.external_power);
    assert(monitor.trend == SOLAR_OS_BATTERY_TREND_UNKNOWN);
    for (uint32_t i = 1U; i <= 60U; i++) {
        provider_sample.battery_mv = (uint16_t)(3820U - i * 2U);
        monitor = sample_monitor(i * 10000U);
    }
    assert(monitor.trend == SOLAR_OS_BATTERY_TREND_DISCHARGING);
    assert(!monitor.external_power && monitor.time_left_valid);
    solar_os_battery_monitor_stop();
}

static void start_voltage_only_monitor(uint16_t mv, uint32_t interval_ms)
{
    provider_sample = (solar_os_battery_sample_t){.battery_mv = mv};
    for (unsigned i = 0U; i < 8U; i++) (void)read_status();
    assert(solar_os_battery_monitor_start(interval_ms) == ESP_OK);
}

static void test_fast_charging_step(uint32_t interval_ms, uint32_t start_ms)
{
    start_voltage_only_monitor(3800U, interval_ms);
    (void)sample_monitor(start_ms);
    /* Interactive status polling must not trigger or advance the detector. */
    provider_sample.battery_mv = 3840U;
    for (unsigned i = 0U; i < 16U; i++) assert(!read_status().charging);
    solar_os_battery_monitor_status_t monitor = sample_monitor(start_ms + interval_ms);
    assert(monitor.external_power && !monitor.external_power_known);
    assert(!monitor.short_window.valid && !monitor.long_window.valid);
    assert(!monitor.time_left_valid);
    assert(monitor.last_voltage_mv == 3840U);
    solar_os_battery_status_t status = read_status();
    assert(status.charging && !status.charging_known);

    /* Disconnect must clear the latch before the old rising regression can
     * reassert it, and must not interpret the voltage step as battery drain. */
    provider_sample.battery_mv = 3800U;
    monitor = sample_monitor(start_ms + interval_ms * 2U);
    assert(!monitor.external_power && !read_status().charging);
    assert(!monitor.short_window.valid && !monitor.time_left_valid);
    solar_os_battery_monitor_stop();
    assert(!read_status().charging);
}

static void test_charging_latch_weak_positive_trend(void)
{
    start_voltage_only_monitor(3800U, 10000U);
    (void)sample_monitor(0U);
    provider_sample.battery_mv = 3840U;
    (void)sample_monitor(10000U);
    solar_os_battery_monitor_status_t monitor;
    /* 30 mV/h is below the 40 mV/h threshold for initially inferring a rise. */
    for (uint32_t i = 1U; i <= 120U; i++) {
        provider_sample.battery_mv = (uint16_t)(3840U + i / 12U);
        monitor = sample_monitor(10000U + i * 10000U);
        assert(monitor.external_power && read_status().charging);
        assert(!monitor.time_left_valid && !monitor.long_window.valid);
    }
    assert(monitor.short_window.valid);
    assert(monitor.trend == SOLAR_OS_BATTERY_TREND_FLAT);
    assert(monitor.short_window.voltage_drop_mvh < 0);
    assert(monitor.short_window.voltage_drop_mvh > -40);

    /* A sustained flat voltage releases the estimate once the slope is zero. */
    for (uint32_t i = 121U; i <= 180U; i++) {
        monitor = sample_monitor(10000U + i * 10000U);
    }
    assert(monitor.short_window.voltage_drop_mvh == 0);
    assert(!monitor.external_power && !read_status().charging);
    solar_os_battery_monitor_stop();
}

static void test_fast_charging_after_discharge(void)
{
    start_voltage_only_monitor(3900U, 10000U);
    solar_os_battery_monitor_status_t monitor;
    for (uint32_t i = 0U; i <= 36U; i++) {
        provider_sample.battery_mv = (uint16_t)(3900U - i);
        monitor = sample_monitor(i * 10000U);
    }
    assert(monitor.trend == SOLAR_OS_BATTERY_TREND_DISCHARGING && monitor.time_left_valid);
    provider_sample.battery_mv += 40U;
    monitor = sample_monitor(370000U);
    assert(monitor.external_power && read_status().charging);
    assert(!monitor.time_left_valid && !monitor.short_window.valid);
    solar_os_battery_monitor_stop();
    assert(!read_status().charging);
    assert(solar_os_battery_monitor_start(10000U) == ESP_OK);
    assert(!sample_monitor(380000U).external_power);
    solar_os_battery_monitor_stop();
}

static void test_charging_latch_slow_reversal(void)
{
    start_voltage_only_monitor(3800U, 10000U);
    (void)sample_monitor(0U);
    provider_sample.battery_mv = 3840U;
    (void)sample_monitor(10000U);
    solar_os_battery_monitor_status_t monitor;
    for (uint32_t i = 1U; i <= 60U; i++) {
        provider_sample.battery_mv = (uint16_t)(3840U - i / 3U);
        monitor = sample_monitor(10000U + i * 10000U);
    }
    assert(!monitor.external_power && !read_status().charging);
    assert(monitor.trend == SOLAR_OS_BATTERY_TREND_DISCHARGING);
    assert(monitor.time_left_valid);
    solar_os_battery_monitor_stop();
}

static void test_fast_charging_noise_and_gaps(void)
{
    start_voltage_only_monitor(3800U, 10000U);
    for (uint32_t i = 0U; i < 40U; i++) {
        provider_sample.battery_mv = (uint16_t)(3800U + i % 5U);
        assert(!sample_monitor(i * 10000U).external_power);
    }
    provider_sample.battery_mv = 3840U;
    assert(sample_monitor(400000U).external_power);
    provider_sample.battery_mv = 3800U;
    assert(!sample_monitor(410000U).external_power);
    assert(!read_status().charging);

    start_voltage_only_monitor(3800U, 180000U);
    (void)sample_monitor(0U);
    provider_sample.battery_mv = 3840U;
    assert(!sample_monitor(180000U).external_power); /* Not a sudden rise. */
    solar_os_battery_monitor_stop();

    start_voltage_only_monitor(3800U, 10000U);
    (void)sample_monitor(0U);
    provider_sample.battery_mv = 3840U;
    assert(!sample_monitor(0U).external_power); /* No elapsed time. */
    solar_os_battery_monitor_stop();
}

static void test_fast_charging_hardware_precedence(void)
{
    provider_sample = (solar_os_battery_sample_t){
        .battery_mv = 3800U, .external_power_valid = true,
        .external_power = true, .charging_valid = true,
    };
    assert(solar_os_battery_monitor_start(10000U) == ESP_OK);
    (void)sample_monitor(0U);
    provider_sample.battery_mv = 3840U;
    (void)sample_monitor(10000U);
    assert(!read_status().charging && read_status().charging_known);
    provider_sample.charging_valid = false;
    assert(read_status().charging && !read_status().charging_known);
    provider_sample.external_power = false;
    assert(!read_status().charging); /* Hardware absence wins immediately. */
    assert(!sample_monitor(20000U).external_power);
    provider_sample.battery_mv = 3880U;
    assert(!sample_monitor(30000U).external_power);
    assert(!read_status().charging);
    solar_os_battery_monitor_stop();
}

static void test_monitor_slow_discharge(void)
{
    provider_sample = (solar_os_battery_sample_t) {
        .battery_mv = 3900U,
        .calibrated = true,
        .external_power_valid = true,
        .charging_valid = true,
    };
    assert(solar_os_battery_monitor_start(10000U) == ESP_OK);
    solar_os_battery_monitor_status_t monitor = {0};
    for (uint32_t i = 0U; i <= 5040U; i++) {
        /* Fourteen hours at 12 mV/h, with ADC noise larger than the
         * five-minute discharge. The last short window is nearly flat. */
        const int noise = (int)(i % 7U) - 3;
        provider_sample.battery_mv = (uint16_t)(3900 - (int)(i / 30U) + noise);
        monitor = sample_monitor(i * 10000U);
        if (i >= 540U) {
            assert(monitor.time_left_valid);
        }
    }
    assert(monitor.sample_count == 5041U);
    assert(monitor.trend == SOLAR_OS_BATTERY_TREND_DISCHARGING);
    assert(monitor.slope_mvh >= -13 && monitor.slope_mvh <= -11);
    assert(monitor.time_left_valid);
    assert(monitor.time_left_min >= 3300U && monitor.time_left_min <= 4000U);
    assert(monitor.full_runtime_min == 1120U * 60U / (uint32_t)(-monitor.slope_mvh));
    assert(stored_runtime_min == monitor.full_runtime_min);
    assert(monitor.short_window.valid);
    assert(monitor.long_window.valid);
    assert(monitor.long_window.span_ms > 13U * 3600000U);
    assert(monitor.long_window.voltage_drop_mvh >= 11 &&
           monitor.long_window.voltage_drop_mvh <= 13);
    assert(monitor.long_window.current_valid);
    assert(monitor.long_window.current_ma ==
           2500 * monitor.long_window.voltage_drop_mvh / 1120);
    assert(!monitor.time_left_seeded);

    assert(solar_os_battery_set_capacity_mah(0U) == ESP_OK);
    solar_os_battery_monitor_get_status(&monitor);
    assert(!monitor.short_window.current_valid && !monitor.long_window.current_valid);
    assert(monitor.short_window.valid && monitor.long_window.valid);
    assert(solar_os_battery_set_capacity_mah(2500U) == ESP_OK);

    /* Hardware-confirmed battery power keeps small rebounds from dropping
     * an established long-term estimate. */
    provider_sample.battery_mv += 12U;
    for (uint32_t i = 5041U; i <= 5071U; i++) {
        monitor = sample_monitor(i * 10000U);
    }
    assert(monitor.time_left_valid);

    /* USB must suppress the estimate and discard the old discharge history. */
    provider_sample.external_power = true;
    monitor = sample_monitor(50720000U);
    assert(monitor.external_power && !monitor.time_left_valid);
    provider_sample.external_power = false;
    for (uint32_t i = 5073U; i <= 5133U; i++) {
        monitor = sample_monitor(i * 10000U);
    }
    assert(!monitor.external_power && monitor.time_left_valid && monitor.time_left_seeded);
    assert(monitor.full_runtime_min == stored_runtime_min);

    /* Voltage-only boards also resolve slow drain, but a recent sustained
     * rise must still take precedence when hardware power is unavailable. */
    provider_sample.external_power_valid = false;
    provider_sample.charging_valid = false;
    assert(solar_os_battery_monitor_start(10000U) == ESP_OK);
    for (uint32_t i = 0U; i <= 5040U; i++) {
        provider_sample.battery_mv = (uint16_t)(3900U - i / 30U);
        monitor = sample_monitor(i * 10000U);
    }
    assert(!monitor.external_power_known && !monitor.external_power);
    assert(monitor.time_left_valid);
    for (uint32_t i = 5041U; i <= 5101U; i++) {
        provider_sample.battery_mv = (uint16_t)(3732U + (i - 5040U) * 2U);
        monitor = sample_monitor(i * 10000U);
    }
    assert(monitor.trend == SOLAR_OS_BATTERY_TREND_CHARGING);
    assert(monitor.external_power && !monitor.time_left_valid);
    for (uint32_t i = 5102U; i <= 5162U; i++) {
        monitor = sample_monitor(i * 10000U);
    }
    assert(!monitor.time_left_valid);
    solar_os_battery_monitor_stop();
}

static void test_monitor_runtime_seed_and_window_stats(void)
{
    assert(solar_os_battery_set_runtime_min(6000U) == ESP_OK);
    assert(stored_runtime_min == 6000U);
    provider_sample = (solar_os_battery_sample_t) {
        .battery_mv = 3560U, /* Half of the configured 3.000--4.120 V range. */
        .external_power_valid = true,
        .charging_valid = true,
    };
    /* Flush the shared voltage average left by earlier tests. */
    for (unsigned i = 0U; i < 8U; i++) {
        (void)read_status();
    }
    assert(solar_os_battery_monitor_start(10000U) == ESP_OK);
    solar_os_battery_monitor_status_t monitor = sample_monitor(0U);
    assert(monitor.time_left_valid && monitor.time_left_seeded);
    assert(monitor.time_left_min == 3000U);
    assert(monitor.full_runtime_min == 6000U);
    assert(!monitor.short_window.valid && !monitor.long_window.valid);

    assert(solar_os_battery_set_runtime_min(1200U) == ESP_OK);
    solar_os_battery_monitor_get_status(&monitor);
    assert(monitor.time_left_seeded && monitor.time_left_min == 600U);
    assert(monitor.full_runtime_min == 1200U);
    assert(solar_os_battery_set_runtime_min(0U) == ESP_OK);
    solar_os_battery_monitor_get_status(&monitor);
    assert(!monitor.time_left_valid && !monitor.time_left_seeded);
    assert(monitor.full_runtime_min == 0U);
    assert(solar_os_battery_set_runtime_min(6000U) == ESP_OK);

    for (uint32_t i = 1U; i <= 60U; i++) {
        provider_sample.battery_mv = (uint16_t)(3560U - i * 2U);
        monitor = sample_monitor(i * 10000U);
    }
    assert(monitor.time_left_valid && !monitor.time_left_seeded);
    assert(monitor.full_runtime_min == 1120U * 60U / (uint32_t)(-monitor.slope_mvh));
    assert(monitor.short_window.valid && monitor.short_window.current_valid);
    assert(monitor.short_window.span_ms == 300000U);
    assert(monitor.short_window.voltage_drop_mvh > 0);
    assert(monitor.short_window.current_ma ==
           2500 * monitor.short_window.voltage_drop_mvh / 1120);

    assert(solar_os_battery_set_capacity_mah(10000U) == ESP_OK);
    solar_os_battery_monitor_get_status(&monitor);
    assert(monitor.short_window.current_ma ==
           10000 * monitor.short_window.voltage_drop_mvh / 1120);
    assert(solar_os_battery_set_capacity_mah(2500U) == ESP_OK);

    provider_sample.external_power = true;
    monitor = sample_monitor(610000U);
    assert(!monitor.time_left_valid && !monitor.time_left_seeded);
    assert(!monitor.long_window.valid);

    for (uint32_t i = 62U; i <= 122U; i++) {
        provider_sample.battery_mv = (uint16_t)(3440U + (i - 61U) * 2U);
        monitor = sample_monitor(i * 10000U);
    }
    assert(monitor.short_window.valid && monitor.short_window.current_valid);
    assert(monitor.short_window.voltage_drop_mvh < 0);
    assert(monitor.short_window.current_ma < 0);
    assert(!monitor.time_left_valid);

    /* Charging also has useful long-window statistics, without an ETA. */
    for (uint32_t i = 1U; i <= 720U; i++) {
        provider_sample.battery_mv = (uint16_t)(3562U + i / 12U);
        monitor = sample_monitor(1220000U + i * 10000U);
    }
    assert(monitor.long_window.valid && monitor.long_window.current_valid);
    assert(monitor.long_window.voltage_drop_mvh < 0);
    assert(monitor.long_window.current_ma < 0);
    assert(!monitor.time_left_valid);
    provider_sample.external_power = false;
    monitor = sample_monitor(8430000U);
    assert(!monitor.long_window.valid);
    assert(!monitor.short_window.valid);

    assert(solar_os_battery_set_runtime_min(SOLAR_OS_BATTERY_RUNTIME_MAX_MIN + 1U) ==
           ESP_ERR_INVALID_ARG);
    assert(stored_runtime_min == 6000U);
    assert(solar_os_battery_set_runtime_min(0U) == ESP_OK);
    assert(stored_runtime_min == 0U);
    solar_os_battery_monitor_stop();
}

static void test_monitor_learned_runtime_persistence(void)
{
    assert(solar_os_battery_set_runtime_min(6000U) == ESP_OK);
    assert(solar_os_battery_set_capacity_mah(0U) == ESP_OK);
    const unsigned writes_before = runtime_write_count;
    const unsigned warnings_before = save_warning_count;
    provider_sample = (solar_os_battery_sample_t) {
        .battery_mv = 3900U,
        .external_power_valid = true,
    };
    assert(solar_os_battery_monitor_start(10000U) == ESP_OK);
    solar_os_battery_monitor_status_t monitor;
    for (uint32_t i = 0U; i < 450U; i++) {
        provider_sample.battery_mv = (uint16_t)(3900U - i / 30U);
        monitor = sample_monitor(i * 10000U);
        assert(runtime_write_count == writes_before);
    }
    assert(monitor.time_left_seeded && monitor.full_runtime_min == 6000U);

    /* The first completed long estimate replaces the manual seed at 75 min. */
    provider_sample.battery_mv = 3885U;
    monitor = sample_monitor(4500000U);
    assert(monitor.long_window.valid && !monitor.time_left_seeded);
    assert(runtime_write_count == writes_before + 1U);
    assert(stored_runtime_min == monitor.full_runtime_min);
    assert(stored_runtime_min != 6000U);
    const uint32_t first_runtime = stored_runtime_min;

    /* Status reads and intervening samples do not save the same estimate. */
    for (unsigned i = 0U; i < 50U; i++) {
        solar_os_battery_monitor_get_status(&monitor);
    }
    for (uint32_t i = 451U; i < 540U; i++) {
        provider_sample.battery_mv = (uint16_t)(3885U - (i - 450U));
        monitor = sample_monitor(i * 10000U);
        assert(runtime_write_count == writes_before + 1U);
    }

    /* A changed estimate saves again after the next fifteen-minute bucket. */
    provider_sample.battery_mv = 3795U;
    monitor = sample_monitor(5400000U);
    assert(runtime_write_count == writes_before + 2U);
    assert(stored_runtime_min == monitor.full_runtime_min);
    assert(stored_runtime_min != first_runtime);
    const uint32_t saved_runtime = stored_runtime_min;

    /* A failed save keeps the previous seed and retries on the next estimate. */
    nvs_write_error = ESP_FAIL;
    for (uint32_t i = 541U; i <= 630U; i++) {
        provider_sample.battery_mv = (uint16_t)(3795U - (i - 540U));
        monitor = sample_monitor(i * 10000U);
    }
    assert(save_warning_count == warnings_before + 1U);
    assert(runtime_write_count == writes_before + 2U);
    assert(stored_runtime_min == saved_runtime);
    assert(monitor.full_runtime_min != saved_runtime);
    solar_os_battery_config_t config;
    solar_os_battery_get_config(&config);
    assert(config.runtime_min == saved_runtime);
    nvs_write_error = ESP_OK;
    for (uint32_t i = 631U; i <= 720U; i++) {
        provider_sample.battery_mv = (uint16_t)(3705U - (i - 630U));
        monitor = sample_monitor(i * 10000U);
    }
    assert(runtime_write_count == writes_before + 3U);
    assert(stored_runtime_min == monitor.full_runtime_min);

    /* Repeated unchanged long estimates do not wear NVS. */
    assert(solar_os_battery_monitor_start(10000U) == ESP_OK);
    provider_sample.battery_mv = 3900U;
    for (unsigned i = 0U; i < 8U; i++) {
        (void)read_status();
    }
    for (uint32_t i = 0U; i <= 450U; i++) {
        provider_sample.battery_mv = (uint16_t)(3900U - i / 30U);
        monitor = sample_monitor(i * 10000U);
    }
    const unsigned unchanged_writes = runtime_write_count;
    for (uint32_t i = 451U; i <= 540U; i++) {
        provider_sample.battery_mv = (uint16_t)(3900U - i / 30U);
        monitor = sample_monitor(i * 10000U);
    }
    assert(runtime_write_count == unchanged_writes);

    /* External power blocks learning even if its voltage is falling. */
    provider_sample.external_power = true;
    for (uint32_t i = 541U; i <= 1081U; i++) {
        provider_sample.battery_mv = (uint16_t)(3882U - (i - 541U) / 30U);
        monitor = sample_monitor(i * 10000U);
    }
    assert(monitor.long_window.valid && monitor.long_window.voltage_drop_mvh > 0);
    assert(!monitor.time_left_valid && monitor.full_runtime_min == 0U);
    assert(runtime_write_count == unchanged_writes);
    provider_sample.external_power = false;
    monitor = sample_monitor(10820000U);
    assert(monitor.time_left_seeded && monitor.full_runtime_min == stored_runtime_min);
    assert(!monitor.long_window.valid);

    assert(solar_os_battery_set_capacity_mah(2500U) == ESP_OK);
    assert(solar_os_battery_set_runtime_min(0U) == ESP_OK);
    solar_os_battery_monitor_stop();
}

int main(void)
{
    solar_os_battery_config_t initial_config;
    solar_os_battery_get_config(&initial_config);
    assert(initial_config.runtime_min == 1440U);
    assert(solar_os_battery_set_runtime_min(0U) == ESP_OK);
#ifdef SOLAR_OS_BOARD_DEFAULT_BATTERY_CAPACITY_MAH
    assert(initial_config.capacity_mah == SOLAR_OS_BOARD_DEFAULT_BATTERY_CAPACITY_MAH);
#else
    assert(initial_config.capacity_mah == 0U);
#endif
    assert(solar_os_battery_set_capacity_mah(2500U) == ESP_OK);
    solar_os_battery_get_config(&initial_config);
    assert(initial_config.capacity_mah == 2500U);
    const solar_os_battery_provider_t provider = {
        .read = read_battery,
        .user = &provider_sample,
    };
    assert(solar_os_battery_register_provider("test-battery", &provider) == ESP_OK);
    assert(solar_os_battery_init() == ESP_OK);
    assert(stream_register_count == 1U);

    provider_sample = (solar_os_battery_sample_t) {
        .battery_mv = 3750U,
        .calibrated = true,
        .external_power_valid = true,
        .external_power = true,
        .charging_valid = true,
        .charging = true,
    };
    solar_os_battery_status_t status = read_status();
    assert(status.external_power);
    assert(status.external_power_known);
    assert(status.charging);
    assert(status.charging_known);
    assert(status.percent_estimated);

    provider_sample = (solar_os_battery_sample_t) {
        .battery_mv = 3750U,
        .calibrated = true,
        .percent_valid = true,
        .percent = 73U,
    };
    status = read_status();
    assert(status.percent == 73U);
    assert(!status.percent_estimated);

    provider_sample.percent = 129U;
    status = read_status();
    assert(status.percent == 100U);
    assert(!status.percent_estimated);

    provider_sample = (solar_os_battery_sample_t) {
        .battery_mv = 5000U,
        .calibrated = true,
        .external_power_valid = true,
        .external_power = false,
        .charging_valid = true,
        .charging = false,
    };
    status = read_status();
    assert(!status.external_power);
    assert(!status.charging);
    assert(status.charging_known);

    provider_sample = (solar_os_battery_sample_t) {
        .battery_mv = 5000U,
        .calibrated = true,
    };
    status = read_status();
    assert(!status.external_power);
    assert(!status.charging);
    assert(!status.charging_known);
    assert(!status.external_power_known);

    test_monitor_hardware_power();
    test_monitor_power_transition_step();
    test_monitor_slow_discharge();
    test_monitor_runtime_seed_and_window_stats();
    test_monitor_learned_runtime_persistence();
    test_fast_charging_step(1000U, 0U);
    test_fast_charging_step(10000U, UINT32_MAX - 5000U);
    test_fast_charging_step(60000U, 0U);
    test_charging_latch_weak_positive_trend();
    test_fast_charging_after_discharge();
    test_charging_latch_slow_reversal();
    test_fast_charging_noise_and_gaps();
    test_fast_charging_hardware_precedence();

    assert(solar_os_battery_unregister_provider("test-battery") == ESP_OK);
    assert(stream_unregister_count == 1U);
    assert(!solar_os_battery_has_provider());

    puts("battery service tests: ok");
    return 0;
}
