#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../../src/jobs/solar_os_batmon_job.c"
#include "../../src/services/solar_os_shutdown.c"

static solar_os_context_t context;
static solar_os_battery_monitor_status_t monitor;
static solar_os_battery_config_t config = {.min_voltage_mv = 3500U};
static esp_err_t start_error, sample_error;
static unsigned starts, samples, stops, sleep_requests, deep_requests;
static uint32_t interval;
static char resource_detail[SOLAR_OS_JOB_RESOURCE_DETAIL_MAX];

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

esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{
    (void)level;
    (void)tag;
    char text[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    return ESP_OK;
}

esp_err_t solar_os_jobs_note_resource(const char *name, solar_os_job_resource_type_t type,
                                       const char *resource, const char *detail)
{
    assert(strcmp(name, "batmon") == 0 && type == SOLAR_OS_JOB_RESOURCE_CUSTOM);
    assert(strcmp(resource, "battery") == 0);
    strlcpy(resource_detail, detail, sizeof(resource_detail));
    return ESP_OK;
}

esp_err_t solar_os_battery_monitor_start(uint32_t interval_ms)
{
    starts++;
    interval = interval_ms;
    return start_error;
}
void solar_os_battery_monitor_stop(void) { stops++; }
esp_err_t solar_os_battery_monitor_sample(uint32_t now_ms)
{
    samples++;
    if (sample_error == ESP_OK) {
        monitor.sample_count++;
        monitor.last_sample_ms = now_ms;
    }
    return sample_error;
}
void solar_os_battery_get_config(solar_os_battery_config_t *out) { *out = config; }
void solar_os_battery_monitor_get_status(solar_os_battery_monitor_status_t *out) { *out = monitor; }
const char *solar_os_battery_trend_name(solar_os_battery_trend_t trend)
{ (void)trend; return "test"; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test"; }
void solar_os_context_request_sleep(solar_os_context_t *ctx)
{ assert(ctx == &context); ctx->sleep_requested = true; sleep_requests++; }
void solar_os_context_request_deep_sleep(solar_os_context_t *ctx)
{ assert(ctx == &context); ctx->deep_sleep_requested = true; deep_requests++; }

static void reset(void)
{
    memset(&batmon, 0, sizeof(batmon));
    memset(&context, 0, sizeof(context));
    monitor = (solar_os_battery_monitor_status_t){
        .last_voltage_mv = 3500U, .trend = SOLAR_OS_BATTERY_TREND_DISCHARGING,
    };
    starts = samples = stops = sleep_requests = deep_requests = 0U;
    start_error = sample_error = ESP_OK;
    resource_detail[0] = '\0';
    atomic_store(&requested, 0U);
    atomic_store(&failure, ESP_OK);
}

static bool tick(uint32_t now_ms)
{
    const solar_os_event_t event = {.type = SOLAR_OS_EVENT_TICK, .data.tick_ms = now_ms};
    return solar_os_batmon_job.event(&context, &event);
}

static void assert_no_action(void)
{
    assert(!context.sleep_requested && !context.deep_sleep_requested);
    assert(!solar_os_shutdown_requested());
}

static void test_arguments(void)
{
    char *defaults[] = {"batmon"};
    reset();
    assert(solar_os_batmon_job.start(&context, 1, defaults) == ESP_OK);
    assert(interval == 60000U && strcmp(resource_detail, "monitor: sleep") == 0);
    solar_os_batmon_job.stop(&context);
    assert(stops == 1U && !tick(0U));

    char *legacy[] = {"batmon", "30"};
    assert(solar_os_batmon_job.start(&context, 2, legacy) == ESP_OK);
    assert(interval == 30000U && strcmp(resource_detail, "monitor: sleep") == 0);
    const char *invalid[] = {"", "9", "86401", "-10", "30x", "off", "hibernate"};
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char *args[] = {"batmon", (char *)invalid[i]};
        const unsigned before = starts;
        assert(solar_os_batmon_job.start(&context, 2, args) == ESP_ERR_INVALID_ARG);
        assert(starts == before);
    }
    char *bad_order[] = {"batmon", "shutdown", "30"};
    char *bad_action[] = {"batmon", "30", "off"};
    char *extra[] = {"batmon", "30", "shutdown", "extra"};
    assert(solar_os_batmon_job.start(&context, 3, bad_order) == ESP_ERR_INVALID_ARG);
    assert(solar_os_batmon_job.start(&context, 3, bad_action) == ESP_ERR_INVALID_ARG);
    assert(solar_os_batmon_job.start(&context, 4, extra) == ESP_ERR_INVALID_ARG);
    char detail[128];
    solar_os_batmon_job.error_detail(detail, sizeof(detail));
    assert(strstr(detail, "sleep|deepsleep|shutdown") != NULL);
    start_error = ESP_ERR_NOT_SUPPORTED;
    assert(solar_os_batmon_job.start(&context, 1, defaults) == ESP_ERR_NOT_SUPPORTED);
    assert(!tick(0U));
    solar_os_batmon_job.error_detail(detail, sizeof(detail));
    assert(strstr(detail, "provider unavailable") != NULL);
}

static void test_actions(void)
{
    const char *actions[] = {"sleep", "deepsleep", "shutdown"};
    for (size_t i = 0U; i < sizeof(actions) / sizeof(actions[0]); i++) {
        reset();
        char *args[] = {"batmon", "10", (char *)actions[i]};
        assert(solar_os_batmon_job.start(&context, 3, args) == ESP_OK);
        assert(interval == 10000U && strstr(resource_detail, actions[i]) != NULL);
        assert(tick(0U));
        assert(!tick(9999U) && samples == 1U);
        assert(tick(10000U));
        assert_no_action();
        assert(tick(20000U));
        assert(sleep_requests == (i == 0U) && deep_requests == (i == 1U));
        assert(solar_os_shutdown_requested() == (i == 2U));
        solar_os_batmon_job.stop(&context);
        assert(!tick(30000U));

        reset();
        char *action_only[] = {"batmon", (char *)actions[i]};
        assert(solar_os_batmon_job.start(&context, 2, action_only) == ESP_OK);
        assert(interval == 60000U && batmon.action == (batmon_action_t)i);
    }
}

static void test_consecutive_samples(void)
{
    reset();
    char *args[] = {"batmon", "10", "shutdown"};
    assert(solar_os_batmon_job.start(&context, 3, args) == ESP_OK);
    uint32_t now = 0U;
    for (int trend = SOLAR_OS_BATTERY_TREND_UNKNOWN; trend <= SOLAR_OS_BATTERY_TREND_CHARGING; trend++) {
        if (trend == SOLAR_OS_BATTERY_TREND_DISCHARGING) continue;
        monitor.trend = (solar_os_battery_trend_t)trend;
        for (unsigned j = 0U; j < 3U; j++, now += 10000U) assert(tick(now));
        assert_no_action();
    }
    monitor.trend = SOLAR_OS_BATTERY_TREND_DISCHARGING;
    for (unsigned reason = 0U; reason < 3U; reason++) {
        assert(tick(now)); now += 10000U;
        assert(tick(now)); now += 10000U;
        monitor.last_voltage_mv = reason == 0U ? 3501U : 3500U;
        monitor.external_power = reason == 1U;
        sample_error = reason == 2U ? ESP_FAIL : ESP_OK;
        assert(tick(now)); now += 10000U;
        assert(batmon.low_voltage_samples == 0U);
        assert_no_action();
        monitor.last_voltage_mv = 3500U;
        monitor.external_power = false;
        sample_error = ESP_OK;
    }
    assert(tick(now)); now += 10000U;
    assert(tick(now)); now += 10000U;
    assert_no_action();
    assert(tick(now) && solar_os_shutdown_requested());
}

int main(void)
{
    test_arguments();
    test_actions();
    test_consecutive_samples();
    puts("batmon: argument compatibility, power actions, timing and consecutive samples: ok");
    return 0;
}
