#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "solar_os_battery_trend.h"

static solar_os_battery_trend_result_t estimate_after_10_second_samples(
    const int16_t *offsets,
    size_t count,
    uint32_t start_ms)
{
    solar_os_battery_trend_estimator_t estimator;
    solar_os_battery_trend_init(&estimator, 10000U);
    solar_os_battery_trend_result_t result = {0};
    for (size_t i = 0; i < count; i++) {
        solar_os_battery_trend_push(&estimator,
                                    start_ms + (uint32_t)i * 10000U,
                                    (uint16_t)(3800 + offsets[i]));
    }
    const bool available = solar_os_battery_trend_estimate(&estimator, &result);
    if (count < 31U) {
        assert(!available);
        assert(result.trend == SOLAR_OS_BATTERY_VOLTAGE_TREND_UNKNOWN);
    } else {
        assert(available);
        assert(result.span_ms >= 300000U);
    }
    return result;
}

static void test_short_noisy_window_is_unknown(void)
{
    const int16_t offsets[] = {0, 2, -1, 3, 1, -2, 2, 4};
    (void)estimate_after_10_second_samples(offsets,
                                           sizeof(offsets) / sizeof(offsets[0]),
                                           0U);
}

static void test_ten_second_noise_is_stable(void)
{
    int16_t offsets[40];
    const int16_t noise[] = {-2, 1, 0, 2, -1, 0, 1, -2};
    for (size_t i = 0; i < 40U; i++) {
        offsets[i] = noise[i % (sizeof(noise) / sizeof(noise[0]))];
    }
    offsets[20] += 30;

    const solar_os_battery_trend_result_t result =
        estimate_after_10_second_samples(offsets, 40U, 0U);
    assert(result.trend == SOLAR_OS_BATTERY_VOLTAGE_TREND_STABLE);
}

static void test_ten_second_rise_and_fall(void)
{
    int16_t rising[40];
    int16_t falling[40];
    for (size_t i = 0; i < 40U; i++) {
        const int16_t movement = (int16_t)(i / 3U);
        rising[i] = movement;
        falling[i] = (int16_t)-movement;
    }

    solar_os_battery_trend_result_t result =
        estimate_after_10_second_samples(rising, 40U, 0U);
    assert(result.trend == SOLAR_OS_BATTERY_VOLTAGE_TREND_RISING);
    assert(result.slope_mvh > 40);
    assert(result.delta_mv >= 5);

    result = estimate_after_10_second_samples(falling, 40U, 0U);
    assert(result.trend == SOLAR_OS_BATTERY_VOLTAGE_TREND_FALLING);
    assert(result.slope_mvh < -40);
    assert(result.delta_mv <= -5);
}

static void test_short_reversal_does_not_flip_direction(void)
{
    int16_t charging[40];
    int16_t discharging[40];
    for (size_t i = 0; i < 40U; i++) {
        int16_t movement = (int16_t)(i / 3U);
        if (i >= 34U) {
            movement = (int16_t)(11 - (int16_t)(i - 33U));
        }
        charging[i] = movement;
        discharging[i] = (int16_t)-movement;
    }

    solar_os_battery_trend_result_t result =
        estimate_after_10_second_samples(charging, 40U, 0U);
    assert(result.trend != SOLAR_OS_BATTERY_VOLTAGE_TREND_FALLING);

    result = estimate_after_10_second_samples(discharging, 40U, 0U);
    assert(result.trend != SOLAR_OS_BATTERY_VOLTAGE_TREND_RISING);
}

static void test_sixty_second_rate_keeps_eight_sample_window(void)
{
    solar_os_battery_trend_estimator_t estimator;
    solar_os_battery_trend_init(&estimator, 60000U);
    for (size_t i = 0; i < 8U; i++) {
        solar_os_battery_trend_push(&estimator,
                                    (uint32_t)i * 60000U,
                                    (uint16_t)(3800U + i * 2U));
    }

    solar_os_battery_trend_result_t result;
    assert(solar_os_battery_trend_estimate(&estimator, &result));
    assert(result.sample_count == 8U);
    assert(result.span_ms == 420000U);
    assert(result.trend == SOLAR_OS_BATTERY_VOLTAGE_TREND_RISING);
}

static void test_tick_wrap(void)
{
    int16_t rising[40];
    for (size_t i = 0; i < 40U; i++) {
        rising[i] = (int16_t)(i / 3U);
    }
    const solar_os_battery_trend_result_t result =
        estimate_after_10_second_samples(rising,
                                         40U,
                                         UINT32_MAX - 150000U);
    assert(result.trend == SOLAR_OS_BATTERY_VOLTAGE_TREND_RISING);
}

static void test_long_history_slow_discharge(uint32_t interval_ms, uint32_t start_ms)
{
    solar_os_battery_trend_estimator_t estimator;
    solar_os_battery_trend_init(&estimator, interval_ms);
    solar_os_battery_trend_result_t result;
    /* Exercise retention beyond sixteen hours and timestamp wraparound. */
    for (uint32_t elapsed = 0U; elapsed <= 24U * 3600000U; elapsed += interval_ms) {
        const int noise = (int)((elapsed / interval_ms) % 7U) - 3;
        solar_os_battery_trend_push(&estimator, start_ms + elapsed,
            (uint16_t)(3900 - (int)(elapsed / 300000U) + noise));
    }
    assert(solar_os_battery_trend_estimate(&estimator, &result));
    assert(result.trend == SOLAR_OS_BATTERY_VOLTAGE_TREND_STABLE);
    assert(solar_os_battery_trend_estimate_long(&estimator, &result));
    assert(result.trend == SOLAR_OS_BATTERY_VOLTAGE_TREND_FALLING);
    assert(result.slope_mvh >= -13 && result.slope_mvh <= -11);
    assert(result.span_ms > 15U * 3600000U && result.span_ms <= 16U * 3600000U);

    solar_os_battery_trend_reset_long(&estimator);
    assert(!solar_os_battery_trend_estimate_long(&estimator, &result));
}

static void test_long_history_noise_and_gaps(void)
{
    solar_os_battery_trend_estimator_t estimator;
    solar_os_battery_trend_init(&estimator, 10000U);
    solar_os_battery_trend_result_t result;
    for (uint32_t i = 0U; i <= 5040U; i++) {
        const int noise = (int)(i % 11U) - 5;
        solar_os_battery_trend_push(&estimator, i * 10000U,
            (uint16_t)(3800 + noise));
        if (i == 180U) {
            assert(!solar_os_battery_trend_estimate_long(&estimator, &result));
        }
    }
    assert(solar_os_battery_trend_estimate_long(&estimator, &result));
    assert(result.trend == SOLAR_OS_BATTERY_VOLTAGE_TREND_STABLE);

    solar_os_battery_trend_push(&estimator, 40U * 3600000U, 3600U);
    assert(!solar_os_battery_trend_estimate_long(&estimator, &result));

    /* Long sampling intervals continue to use the original short estimator. */
    solar_os_battery_trend_init(&estimator, 86400000U);
    for (uint32_t i = 0U; i < 60U; i++) {
        solar_os_battery_trend_push(&estimator, i * 86400000U, 3800U);
    }
    assert(!solar_os_battery_trend_estimate_long(&estimator, &result));
}

int main(void)
{
    test_short_noisy_window_is_unknown();
    test_ten_second_noise_is_stable();
    test_ten_second_rise_and_fall();
    test_short_reversal_does_not_flip_direction();
    test_sixty_second_rate_keeps_eight_sample_window();
    test_tick_wrap();
    test_long_history_slow_discharge(10000U, 0U);
    test_long_history_slow_discharge(60000U, UINT32_MAX - 6U * 3600000U);
    test_long_history_noise_and_gaps();
    puts("battery trend tests: ok");
    return 0;
}
