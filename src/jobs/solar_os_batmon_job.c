#include "solar_os_batmon_job.h"

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_battery.h"
#include "solar_os_shutdown.h"

#define BATMON_DEFAULT_INTERVAL_SEC 60U
#define BATMON_MIN_INTERVAL_SEC 10U
#define BATMON_MAX_INTERVAL_SEC 86400U
#define BATMON_LOW_VOLTAGE_SAMPLE_LIMIT 3U

static const char *TAG = "solar_os_batmon";

typedef enum {
    BATMON_ACTION_SLEEP,
    BATMON_ACTION_DEEPSLEEP,
    BATMON_ACTION_SHUTDOWN,
} batmon_action_t;

static const char *const batmon_action_names[] = {"sleep", "deepsleep", "shutdown"};

typedef struct {
    bool running;
    batmon_action_t action;
    uint32_t interval_ms;
    uint32_t next_sample_ms;
    uint32_t success_count;
    uint32_t fail_count;
    uint8_t low_voltage_samples;
    bool low_voltage_action_requested;
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

static bool batmon_parse_action(const char *text, batmon_action_t *action)
{
    if (text == NULL) {
        return false;
    }
    for (size_t i = 0U; i < sizeof(batmon_action_names) / sizeof(batmon_action_names[0]); i++) {
        if (strcmp(text, batmon_action_names[i]) == 0) {
            *action = (batmon_action_t)i;
            return true;
        }
    }
    return false;
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

    const char *power = "unknown";
    if (status.trend == SOLAR_OS_BATTERY_TREND_CHARGING &&
        status.external_power) {
        power = "external";
    } else if (status.trend == SOLAR_OS_BATTERY_TREND_DISCHARGING) {
        power = "battery";
    }

    SOLAR_OS_LOGI(TAG,
             "%u.%03u V %u%% trend=%s power=%s slope=%" PRId32 " mV/h eta=%s samples=%" PRIu32,
             (unsigned)(status.last_voltage_mv / 1000U),
             (unsigned)(status.last_voltage_mv % 1000U),
             (unsigned)status.last_percent,
             solar_os_battery_trend_name(status.trend),
             power,
             status.slope_mvh,
             eta,
             status.sample_count);
}

static void batmon_check_low_voltage(solar_os_context_t *ctx)
{
    solar_os_battery_config_t config;
    solar_os_battery_monitor_status_t status;

    solar_os_battery_get_config(&config);
    solar_os_battery_monitor_get_status(&status);
    if (status.sample_count == 0 || status.last_voltage_mv > config.min_voltage_mv) {
        batmon.low_voltage_samples = 0;
        batmon.low_voltage_action_requested = false;
        return;
    }
    if (status.trend != SOLAR_OS_BATTERY_TREND_DISCHARGING ||
        status.external_power) {
        batmon.low_voltage_samples = 0;
        batmon.low_voltage_action_requested = false;
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

    if (!batmon.low_voltage_action_requested) {
        SOLAR_OS_LOGW(TAG,
                 "low voltage persisted for %u samples: %u.%03u V <= min %u.%03u V; requesting %s",
                 (unsigned)BATMON_LOW_VOLTAGE_SAMPLE_LIMIT,
                 (unsigned)(status.last_voltage_mv / 1000U),
                 (unsigned)(status.last_voltage_mv % 1000U),
                 (unsigned)(config.min_voltage_mv / 1000U),
                 (unsigned)(config.min_voltage_mv % 1000U),
                 batmon_action_names[batmon.action]);
    }

    batmon.low_voltage_action_requested = true;
    switch (batmon.action) {
    case BATMON_ACTION_SLEEP:
        solar_os_context_request_sleep(ctx);
        break;
    case BATMON_ACTION_DEEPSLEEP:
        solar_os_context_request_deep_sleep(ctx);
        break;
    case BATMON_ACTION_SHUTDOWN:
        (void)solar_os_shutdown_request();
        break;
    }
}

static esp_err_t batmon_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    batmon.last_error = ESP_ERR_INVALID_ARG;

    if (argc < 1 || argc > 3 || argv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t interval_sec = BATMON_DEFAULT_INTERVAL_SEC;
    batmon_action_t action = BATMON_ACTION_SLEEP;
    if (argc >= 2 && !batmon_parse_interval(argv[1], &interval_sec) &&
        (argc != 2 || !batmon_parse_action(argv[1], &action))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (argc == 3 && !batmon_parse_action(argv[2], &action)) {
        return ESP_ERR_INVALID_ARG;
    }

    batmon.last_error = solar_os_battery_monitor_start(interval_sec * 1000U);
    if (batmon.last_error != ESP_OK) {
        batmon.running = false;
        return batmon.last_error;
    }
    batmon.running = true;
    batmon.action = action;
    batmon.interval_ms = interval_sec * 1000U;
    batmon.next_sample_ms = 0;
    batmon.success_count = 0;
    batmon.fail_count = 0;
    batmon.low_voltage_samples = 0;
    batmon.low_voltage_action_requested = false;

    SOLAR_OS_LOGI(TAG, "started: interval=%" PRIu32 "s action=%s",
                  interval_sec, batmon_action_names[action]);
    char label[32];
    snprintf(label, sizeof(label), "monitor: %s", batmon_action_names[action]);
    (void)solar_os_jobs_note_resource(solar_os_batmon_job.name,
                                      SOLAR_OS_JOB_RESOURCE_CUSTOM,
                                      "battery",
                                      label);
    return batmon.last_error;
}

static void batmon_error_detail(char *buffer, size_t buffer_len)
{
    if (buffer != NULL && buffer_len > 0U) {
        strlcpy(buffer, batmon.last_error == ESP_ERR_NOT_SUPPORTED ?
                    "battery provider unavailable; check expansion status and boot logs" :
                    batmon.last_error == ESP_ERR_INVALID_ARG ?
                    "usage: job start batmon [interval-sec] [sleep|deepsleep|shutdown]" :
                    "", buffer_len);
    }
}

static void batmon_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    batmon.running = false;
    batmon.low_voltage_samples = 0;
    batmon.low_voltage_action_requested = false;
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
        batmon_check_low_voltage(ctx);
    } else {
        batmon.fail_count++;
        batmon.low_voltage_samples = 0;
        batmon.low_voltage_action_requested = false;
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
    .error_detail = batmon_error_detail,
    .tick_interval_ms = 250U,
    .tick_deadline_ms = 10U,
};
