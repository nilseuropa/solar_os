#include "solar_os_batmon_job.h"

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_battery.h"

#define BATMON_DEFAULT_INTERVAL_SEC 60U
#define BATMON_MIN_INTERVAL_SEC 10U
#define BATMON_MAX_INTERVAL_SEC 86400U
#define BATMON_LOW_VOLTAGE_SAMPLE_LIMIT 3U

static const char *TAG = "solar_os_batmon";

typedef struct {
    bool running;
    uint32_t interval_ms;
    uint32_t next_sample_ms;
    uint32_t success_count;
    uint32_t fail_count;
    uint8_t low_voltage_samples;
    bool low_voltage_sleep_requested;
    esp_err_t last_error;
} batmon_job_state_t;

static batmon_job_state_t batmon = {
    .interval_ms = BATMON_DEFAULT_INTERVAL_SEC * 1000U,
    .last_error = ESP_OK,
};

static bool batmon_parse_interval(const char *text, uint32_t *seconds)
{
    if (text == NULL || text[0] == '\0' || seconds == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        parsed < BATMON_MIN_INTERVAL_SEC ||
        parsed > BATMON_MAX_INTERVAL_SEC) {
        return false;
    }

    *seconds = (uint32_t)parsed;
    return true;
}

static void batmon_format_minutes(uint32_t minutes, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }

    const uint32_t days = minutes / (24U * 60U);
    minutes %= 24U * 60U;
    const uint32_t hours = minutes / 60U;
    const uint32_t mins = minutes % 60U;

    if (days > 0) {
        snprintf(buffer, buffer_len, "%" PRIu32 "d %" PRIu32 "h", days, hours);
    } else if (hours > 0) {
        snprintf(buffer, buffer_len, "%" PRIu32 "h %" PRIu32 "m", hours, mins);
    } else {
        snprintf(buffer, buffer_len, "%" PRIu32 "m", mins);
    }
}

static void batmon_log_status(void)
{
    solar_os_battery_monitor_status_t status;
    solar_os_battery_monitor_get_status(&status);

    char eta[24];
    if (status.time_left_valid) {
        batmon_format_minutes(status.time_left_min, eta, sizeof(eta));
    } else {
        strlcpy(eta, "unknown", sizeof(eta));
    }

    SOLAR_OS_LOGI(TAG,
             "%u.%03u V %u%% trend=%s power=%s slope=%" PRId32 " mV/h eta=%s samples=%" PRIu32,
             (unsigned)(status.last_voltage_mv / 1000U),
             (unsigned)(status.last_voltage_mv % 1000U),
             (unsigned)status.last_percent,
             solar_os_battery_trend_name(status.trend),
             status.external_power ? "external" : "battery",
             status.slope_mvh,
             eta,
             status.sample_count);
}

static void batmon_check_low_voltage_sleep(solar_os_context_t *ctx)
{
    solar_os_battery_config_t config;
    solar_os_battery_monitor_status_t status;

    solar_os_battery_get_config(&config);
    solar_os_battery_monitor_get_status(&status);
    if (status.sample_count == 0 || status.last_voltage_mv > config.min_voltage_mv) {
        batmon.low_voltage_samples = 0;
        batmon.low_voltage_sleep_requested = false;
        return;
    }
    if (status.external_power) {
        batmon.low_voltage_samples = 0;
        batmon.low_voltage_sleep_requested = false;
        return;
    }

    if (batmon.low_voltage_samples < BATMON_LOW_VOLTAGE_SAMPLE_LIMIT) {
        batmon.low_voltage_samples++;
    }
    if (batmon.low_voltage_samples < BATMON_LOW_VOLTAGE_SAMPLE_LIMIT) {
        SOLAR_OS_LOGW(TAG,
                 "low voltage sample %u/%u: %u.%03u V <= min %u.%03u V",
                 (unsigned)batmon.low_voltage_samples,
                 (unsigned)BATMON_LOW_VOLTAGE_SAMPLE_LIMIT,
                 (unsigned)(status.last_voltage_mv / 1000U),
                 (unsigned)(status.last_voltage_mv % 1000U),
                 (unsigned)(config.min_voltage_mv / 1000U),
                 (unsigned)(config.min_voltage_mv % 1000U));
        return;
    }

    if (!batmon.low_voltage_sleep_requested) {
        SOLAR_OS_LOGW(TAG,
                 "low voltage persisted for %u samples: %u.%03u V <= min %u.%03u V; requesting sleep",
                 (unsigned)BATMON_LOW_VOLTAGE_SAMPLE_LIMIT,
                 (unsigned)(status.last_voltage_mv / 1000U),
                 (unsigned)(status.last_voltage_mv % 1000U),
                 (unsigned)(config.min_voltage_mv / 1000U),
                 (unsigned)(config.min_voltage_mv % 1000U));
    }

    batmon.low_voltage_sleep_requested = true;
    solar_os_context_request_sleep(ctx);
}

static esp_err_t batmon_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;

    if (argc > 2) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t interval_sec = BATMON_DEFAULT_INTERVAL_SEC;
    if (argc == 2 && !batmon_parse_interval(argv[1], &interval_sec)) {
        return ESP_ERR_INVALID_ARG;
    }

    batmon.running = true;
    batmon.interval_ms = interval_sec * 1000U;
    batmon.next_sample_ms = 0;
    batmon.success_count = 0;
    batmon.fail_count = 0;
    batmon.low_voltage_samples = 0;
    batmon.low_voltage_sleep_requested = false;
    batmon.last_error = solar_os_battery_monitor_start(batmon.interval_ms);

    SOLAR_OS_LOGI(TAG, "started: interval=%" PRIu32 "s", interval_sec);
    (void)solar_os_jobs_note_resource(solar_os_batmon_job.name,
                                      SOLAR_OS_JOB_RESOURCE_CUSTOM,
                                      "battery",
                                      "monitor");
    return batmon.last_error;
}

static void batmon_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    batmon.running = false;
    batmon.low_voltage_samples = 0;
    batmon.low_voltage_sleep_requested = false;
    solar_os_battery_monitor_stop();
    SOLAR_OS_LOGI(TAG,
             "stopped: ok=%" PRIu32 " fail=%" PRIu32,
             batmon.success_count,
             batmon.fail_count);
}

static bool batmon_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;

    if (!batmon.running || event == NULL || event->type != SOLAR_OS_EVENT_TICK) {
        return false;
    }

    const uint32_t now_ms = event->data.tick_ms;
    if (batmon.next_sample_ms != 0 &&
        (int32_t)(now_ms - batmon.next_sample_ms) < 0) {
        return false;
    }

    batmon.next_sample_ms = now_ms + batmon.interval_ms;
    const esp_err_t err = solar_os_battery_monitor_sample(now_ms);
    batmon.last_error = err;
    if (err == ESP_OK) {
        batmon.success_count++;
        batmon_log_status();
        batmon_check_low_voltage_sleep(ctx);
    } else {
        batmon.fail_count++;
        batmon.low_voltage_samples = 0;
        batmon.low_voltage_sleep_requested = false;
        SOLAR_OS_LOGW(TAG, "sample failed: %s", esp_err_to_name(err));
    }
    return true;
}

const solar_os_job_t solar_os_batmon_job = {
    .name = "batmon",
    .summary = "battery voltage trend monitor",
    .start = batmon_start,
    .stop = batmon_stop,
    .event = batmon_event,
    .tick_interval_ms = 250U,
    .tick_deadline_ms = 10U,
};
