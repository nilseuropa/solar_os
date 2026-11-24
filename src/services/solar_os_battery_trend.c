#include "solar_os_battery_trend.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BATTERY_TREND_MIN_SPAN_MS (5U * 60U * 1000U)
#define BATTERY_TREND_BASE_INTERVALS 7U
#define BATTERY_TREND_MIN_SAMPLES 4U
#define BATTERY_TREND_SLOPE_THRESHOLD_MVH 40
#define BATTERY_TREND_DELTA_THRESHOLD_MV 5
#define BATTERY_TREND_ENDPOINT_SAMPLES 3U
#define BATTERY_TREND_LONG_BUCKET_MS (15U * 60U * 1000U)
#define BATTERY_TREND_LONG_MIN_SPAN_MS (60U * 60U * 1000U)
#define BATTERY_TREND_LONG_MAX_SPAN_MS (16U * 60U * 60U * 1000U)

void solar_os_battery_trend_reset_long(
    solar_os_battery_trend_estimator_t *estimator)
{
    if (estimator != NULL) {
        estimator->long_sample_count = 0U;
        estimator->bucket_count = 0U;
        estimator->bucket_sum_mv = 0U;
        estimator->bucket_sum_ms = 0U;
    }
}

void solar_os_battery_trend_init(
    solar_os_battery_trend_estimator_t *estimator,
    uint32_t interval_ms)
{
    if (estimator == NULL) {
        return;
    }

    memset(estimator, 0, sizeof(*estimator));
    estimator->interval_ms = interval_ms;
}

static void battery_trend_push_long(
    solar_os_battery_trend_estimator_t *estimator,
    uint32_t tick_ms,
    uint16_t voltage_mv)
{
    if (estimator->bucket_count > 0U &&
        tick_ms - estimator->bucket_start_ms >= BATTERY_TREND_LONG_BUCKET_MS) {
        if (estimator->long_sample_count == SOLAR_OS_BATTERY_TREND_LONG_HISTORY) {
            memmove(&estimator->long_samples[0],
                    &estimator->long_samples[1],
                    sizeof(estimator->long_samples[0]) *
                        (SOLAR_OS_BATTERY_TREND_LONG_HISTORY - 1U));
            estimator->long_sample_count--;
        }
        /* Pair the averaged voltage with the averaged sample time, including
         * uneven sampling and uptime wraparound. */
        estimator->long_samples[estimator->long_sample_count++] =
            (solar_os_battery_trend_sample_t) {
                .tick_ms = estimator->bucket_start_ms +
                    (uint32_t)(estimator->bucket_sum_ms / estimator->bucket_count),
                .voltage_mv = (uint16_t)((estimator->bucket_sum_mv +
                    estimator->bucket_count / 2U) / estimator->bucket_count),
            };
        estimator->bucket_count = 0U;
        estimator->bucket_sum_mv = 0U;
        estimator->bucket_sum_ms = 0U;
    }

    /* Bound sample age as well as count, so sparse sampling and long gaps do
     * not retain stale data or exceed the regression's arithmetic range. */
    size_t expired = 0U;
    while (expired < estimator->long_sample_count &&
           tick_ms - estimator->long_samples[expired].tick_ms >
               BATTERY_TREND_LONG_MAX_SPAN_MS) {
        expired++;
    }
    if (expired > 0U) {
        estimator->long_sample_count -= expired;
        memmove(&estimator->long_samples[0],
                &estimator->long_samples[expired],
                sizeof(estimator->long_samples[0]) * estimator->long_sample_count);
    }

    if (estimator->bucket_count == 0U) {
        estimator->bucket_start_ms = tick_ms;
    }
    estimator->bucket_sum_mv += voltage_mv;
    estimator->bucket_sum_ms += tick_ms - estimator->bucket_start_ms;
    estimator->bucket_count++;
}

void solar_os_battery_trend_push(
    solar_os_battery_trend_estimator_t *estimator,
    uint32_t tick_ms,
    uint16_t voltage_mv)
{
    if (estimator == NULL) {
        return;
    }

    battery_trend_push_long(estimator, tick_ms, voltage_mv);

    if (estimator->sample_count < SOLAR_OS_BATTERY_TREND_HISTORY) {
        estimator->samples[estimator->sample_count++] =
            (solar_os_battery_trend_sample_t) {
                .tick_ms = tick_ms,
                .voltage_mv = voltage_mv,
            };
        return;
    }

    memmove(&estimator->samples[0],
            &estimator->samples[1],
            sizeof(estimator->samples[0]) *
                (SOLAR_OS_BATTERY_TREND_HISTORY - 1U));
    estimator->samples[SOLAR_OS_BATTERY_TREND_HISTORY - 1U] =
        (solar_os_battery_trend_sample_t) {
            .tick_ms = tick_ms,
            .voltage_mv = voltage_mv,
        };
}

static uint32_t battery_trend_target_span_ms(uint32_t interval_ms)
{
    const uint64_t interval_span =
        (uint64_t)interval_ms * BATTERY_TREND_BASE_INTERVALS;
    uint64_t target = interval_span > BATTERY_TREND_MIN_SPAN_MS ?
        interval_span : BATTERY_TREND_MIN_SPAN_MS;
    if (target > UINT32_MAX / 2U) {
        target = UINT32_MAX / 2U;
    }
    return (uint32_t)target;
}

static bool battery_trend_select_window(
    const solar_os_battery_trend_estimator_t *estimator,
    size_t *start,
    uint32_t *span_ms)
{
    if (estimator->sample_count < BATTERY_TREND_MIN_SAMPLES) {
        return false;
    }

    const size_t newest = estimator->sample_count - 1U;
    const uint32_t target_span =
        battery_trend_target_span_ms(estimator->interval_ms);
    size_t selected = newest;
    uint32_t selected_span = 0;
    while (selected > 0U) {
        selected--;
        selected_span = estimator->samples[newest].tick_ms -
            estimator->samples[selected].tick_ms;
        if (selected_span >= target_span &&
            newest - selected + 1U >= BATTERY_TREND_MIN_SAMPLES) {
            break;
        }
    }

    if (selected_span < target_span ||
        newest - selected + 1U < BATTERY_TREND_MIN_SAMPLES) {
        return false;
    }

    *start = selected;
    *span_ms = selected_span;
    return true;
}

static bool battery_trend_calculate_slope(
    const solar_os_battery_trend_sample_t *samples,
    size_t count,
    int32_t *slope_mvh)
{
    const uint32_t base_ms = samples[0].tick_ms;
    int64_t sum_t = 0;
    int64_t sum_v = 0;
    int64_t sum_tt = 0;
    int64_t sum_tv = 0;

    for (size_t i = 0U; i < count; i++) {
        const int64_t t =
            (int64_t)((samples[i].tick_ms - base_ms) / 1000U);
        const int64_t v = samples[i].voltage_mv;
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
    *slope_mvh = (int32_t)((numerator * 3600LL) / denominator);
    return true;
}

static int32_t battery_trend_endpoint_delta(
    const solar_os_battery_trend_estimator_t *estimator,
    size_t start)
{
    const size_t count = estimator->sample_count - start;
    const size_t endpoint_count = count >= BATTERY_TREND_ENDPOINT_SAMPLES * 2U ?
        BATTERY_TREND_ENDPOINT_SAMPLES : 1U;
    uint32_t first_sum = 0;
    uint32_t last_sum = 0;
    for (size_t i = 0; i < endpoint_count; i++) {
        first_sum += estimator->samples[start + i].voltage_mv;
        last_sum += estimator->samples[
            estimator->sample_count - endpoint_count + i].voltage_mv;
    }

    const int32_t first = (int32_t)((first_sum + endpoint_count / 2U) /
                                    endpoint_count);
    const int32_t last = (int32_t)((last_sum + endpoint_count / 2U) /
                                   endpoint_count);
    return last - first;
}

bool solar_os_battery_trend_estimate(
    const solar_os_battery_trend_estimator_t *estimator,
    solar_os_battery_trend_result_t *result)
{
    if (estimator == NULL || result == NULL) {
        return false;
    }

    memset(result, 0, sizeof(*result));
    result->trend = SOLAR_OS_BATTERY_VOLTAGE_TREND_UNKNOWN;

    size_t start = 0;
    if (!battery_trend_select_window(estimator, &start, &result->span_ms) ||
        !battery_trend_calculate_slope(&estimator->samples[start],
                                       estimator->sample_count - start,
                                       &result->slope_mvh)) {
        return false;
    }

    result->sample_count = estimator->sample_count - start;
    result->delta_mv = battery_trend_endpoint_delta(estimator, start);
    if (result->slope_mvh >= BATTERY_TREND_SLOPE_THRESHOLD_MVH &&
        result->delta_mv >= BATTERY_TREND_DELTA_THRESHOLD_MV) {
        result->trend = SOLAR_OS_BATTERY_VOLTAGE_TREND_RISING;
    } else if (result->slope_mvh <= -BATTERY_TREND_SLOPE_THRESHOLD_MVH &&
               result->delta_mv <= -BATTERY_TREND_DELTA_THRESHOLD_MV) {
        result->trend = SOLAR_OS_BATTERY_VOLTAGE_TREND_FALLING;
    } else {
        result->trend = SOLAR_OS_BATTERY_VOLTAGE_TREND_STABLE;
    }
    return true;
}

bool solar_os_battery_trend_estimate_long(
    const solar_os_battery_trend_estimator_t *estimator,
    solar_os_battery_trend_result_t *result)
{
    if (estimator == NULL || result == NULL) {
        return false;
    }
    memset(result, 0, sizeof(*result));
    result->trend = SOLAR_OS_BATTERY_VOLTAGE_TREND_UNKNOWN;
    const size_t count = estimator->long_sample_count;
    if (count < BATTERY_TREND_MIN_SAMPLES) {
        return false;
    }
    const solar_os_battery_trend_sample_t *samples = estimator->long_samples;
    result->span_ms = samples[count - 1U].tick_ms - samples[0].tick_ms;
    if (result->span_ms < BATTERY_TREND_LONG_MIN_SPAN_MS ||
        !battery_trend_calculate_slope(samples, count, &result->slope_mvh)) {
        return false;
    }
    result->sample_count = count;
    result->delta_mv = (int32_t)samples[count - 1U].voltage_mv - samples[0].voltage_mv;
    const int64_t fitted_delta_mv =
        (int64_t)result->slope_mvh * result->span_ms / 3600000LL;
    result->trend = SOLAR_OS_BATTERY_VOLTAGE_TREND_STABLE;
    /* Require a measurable drop across an hour or more, rather than the
     * short window's 40 mV/h cutoff, to resolve slow drain above ADC noise. */
    if (fitted_delta_mv <= -BATTERY_TREND_DELTA_THRESHOLD_MV &&
        result->delta_mv <= -BATTERY_TREND_DELTA_THRESHOLD_MV) {
        result->trend = SOLAR_OS_BATTERY_VOLTAGE_TREND_FALLING;
    }
    return true;
}
