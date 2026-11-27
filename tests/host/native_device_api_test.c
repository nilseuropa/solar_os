#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "solar_os_native_client.h"
#include "solar_os_native_device.h"
#include "solar_os_config.h"
#include "solar_os_memory.h"
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return calloc(count, size); }
void solar_os_memory_free(void *p) { free(p); }
static unsigned sensor_provider_pins;
int solar_os_native_library_retain(uint32_t source, uint32_t *out)
{
    *out = 0;
    if (source != 123) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *out = 124;
    ++sensor_provider_pins;
    return SOLAR_OS_NATIVE_OK;
}
int solar_os_native_library_release(uint32_t lease)
{
    assert(lease == 124 && sensor_provider_pins);
    --sensor_provider_pins;
    return SOLAR_OS_NATIVE_OK;
}
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include <pthread.h>
#include <stdatomic.h>
#include "nvs.h"
#include "solar_os_board_caps.h"
#include "solar_os_backlight.h"
#include "solar_os_identity.h"
#include "solar_os_log.h"
#include "solar_os_power.h"
#include "solar_os_shutdown.h"
#include "solar_os_storage.h"
#include "esp_sleep.h"

static char identity_store_user[32] = "invalid user", identity_store_host[32] = "saved-host";
static char identity_pending[32];
static char *identity_store_target;
static atomic_uint identity_nvs_open;
static bool identity_nvs_unavailable, identity_store_failure;

size_t strlcpy(char *out, const char *value, size_t capacity)
{
    const size_t length = strlen(value);
    if (capacity) {
        const size_t copied = length < capacity - 1 ? length : capacity - 1;
        memcpy(out, value, copied);
        out[copied] = 0;
    }
    return length;
}

static char *identity_storage_field(const char *key)
{
    if (!strcmp(key, "user")) return identity_store_user;
    assert(!strcmp(key, "hostname"));
    return identity_store_host;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "identity") && mode == NVS_READWRITE);
    if (identity_nvs_unavailable) return ESP_FAIL;
    assert(atomic_fetch_add(&identity_nvs_open, 1) == 0);
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *length)
{
    assert(handle == 1 && atomic_load(&identity_nvs_open) == 1);
    const char *saved = identity_storage_field(key);
    assert(strlen(saved) < *length);
    *length = strlcpy(value, saved, *length) + 1;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    assert(handle == 1 && !identity_store_target && atomic_load(&identity_nvs_open) == 1);
    identity_store_target = identity_storage_field(key);
    assert(strlcpy(identity_pending, value, sizeof(identity_pending)) < sizeof(identity_pending));
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1 && identity_store_target);
    if (identity_store_failure) return ESP_FAIL;
    strcpy(identity_store_target, identity_pending);
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1);
    identity_store_target = NULL;
    assert(atomic_fetch_sub(&identity_nvs_open, 1) == 1);
}

bool solar_os_storage_is_mounted(void) { return false; }
esp_err_t solar_os_storage_default_path(const char *name, char *out, size_t capacity)
{ (void)name; (void)out; (void)capacity; return ESP_ERR_NOT_FOUND; }
esp_err_t solar_os_storage_join_path(const char *parent, const char *name, char *out, size_t capacity)
{ (void)parent; (void)name; (void)out; (void)capacity; return ESP_ERR_NOT_FOUND; }
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{ (void)level; (void)tag; (void)fmt; return ESP_OK; }

static void *identity_worker(void *value)
{
    for (unsigned i = 0; i < 32; i++) assert(solar_os_identity_set_user(value) == ESP_OK);
    return NULL;
}

static void verify_identity(void)
{
    const solar_os_native_identity_api_v1_t *api =
        solar_os_native_device_get_service("device.identity", 1, sizeof(*api));
    assert(api && api->get(NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    solar_os_identity_v1_t identity;
    identity_nvs_unavailable = true;
    assert(!api->get(&identity) && !strcmp(identity.user, "user") && !strcmp(identity.hostname, "sol"));
    identity_nvs_unavailable = false;
    assert(!api->get(&identity) && !strcmp(identity.user, "user") && !strcmp(identity.hostname, "saved-host"));
    assert(api->set(SOLAR_OS_IDENTITY_USER, "invalid user") == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->set(SOLAR_OS_IDENTITY_HOSTNAME, "01234567890123456789012345678901") ==
           SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->set((solar_os_identity_field_t)2, "valid") == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);

    char format[64], short_text[5];
    assert(!api->set(SOLAR_OS_IDENTITY_HOSTNAME, "workbench"));
    assert(!strcmp(identity_store_host, "workbench"));
    assert(!strcmp(identity.hostname, "saved-host"));
    assert(solar_os_identity_set_user("operator") == ESP_OK);
    api->format(format, sizeof(format));
    assert(!strcmp(format, "operator@workbench"));
    api->format(short_text, sizeof(short_text));
    assert(!strcmp(short_text, "oper"));

    identity_store_failure = true;
    assert(api->set(SOLAR_OS_IDENTITY_USER, "another") == SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(!api->get(&identity) && !strcmp(identity.user, "operator"));
    identity_store_failure = false;

    pthread_t first, second;
    char first_user[] = "first", second_user[] = "second";
    assert(!pthread_create(&first, NULL, identity_worker, first_user));
    assert(!pthread_create(&second, NULL, identity_worker, second_user));
    assert(!pthread_join(first, NULL) && !pthread_join(second, NULL));
    assert(!api->get(&identity) && !strcmp(identity.user, identity_store_user));
}
static char board_id[100] = "host";
static uint64_t hardware_caps = SOLAR_OS_BOARD_CAP_CAMERA | SOLAR_OS_BOARD_CAP_EXPANSION_I2S;
solar_os_board_info_t solar_os_board_info(void)
{ return (solar_os_board_info_t){.id = board_id, .name = "Host board", .module = "S3", .capabilities = hardware_caps, .psram_bytes = 8388608}; }
solar_os_board_capabilities_t solar_os_board_capabilities(void) { return hardware_caps; }
bool solar_os_board_has(solar_os_board_capability_t value) { return (value & hardware_caps) != 0; }
const char *solar_os_board_capability_name(solar_os_board_capability_t value)
{ assert(value == SOLAR_OS_BOARD_CAP_CAMERA); return "camera"; }
bool solar_os_board_capabilities_format(char *out, size_t capacity)
{ if (capacity < sizeof("camera")) return false; memcpy(out, "camera", sizeof("camera")); return true; }
esp_err_t solar_os_backlight_get_status(solar_os_backlight_status_t *out)
{ *out = (solar_os_backlight_status_t){.follow_display = true, .suspended = true, .percent = 73, .effective_percent = 0, .providers = 2, .last_error = ESP_ERR_TIMEOUT}; return ESP_OK; }
esp_err_t solar_os_backlight_set_brightness(uint8_t value) { assert(value == 100); return ESP_OK; }
esp_err_t solar_os_backlight_display_changed(uint8_t value) { assert(value == 0); return ESP_OK; }
esp_err_t solar_os_backlight_set_follow(void) { return ESP_OK; }
esp_err_t solar_os_backlight_toggle(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_backlight_set_suspended(bool value) { assert(value); return ESP_OK; }
esp_err_t solar_os_backlight_register(solar_os_backlight_set_fn_t set, void *user)
{ (void)set; (void)user; return ESP_ERR_NOT_SUPPORTED; }
void solar_os_backlight_unregister(void *user) { (void)user; }
static solar_os_power_status_t power_value = {
    .initialized = true, .profile = SOLAR_OS_POWER_PROFILE_LOWPOWER, .effective_profile = SOLAR_OS_POWER_PROFILE_BALANCED,
    .key_action = SOLAR_OS_POWER_KEY_ACTION_SHUTDOWN, .key_long_action = SOLAR_OS_POWER_KEY_ACTION_DEEPSLEEP,
    .idle_sleep_ms = UINT32_MAX, .cpu_min_mhz = 80, .cpu_max_mhz = 160, .automatic_light_sleep = true,
    .explicit_sleep_active = true, .suspend_active = true, .automatic_light_sleep_holdoff_ms = 23,
    .pm_configured = true, .pm_last_error = ESP_ERR_TIMEOUT, .bt_sleep_enabled = true, .bt_sleep_last_error = ESP_ERR_NOT_SUPPORTED,
    .light_sleep_count = 7, .last_activity_ms = 99, .last_sleep_duration_ms = 13,
    .last_wakeup_cause = ESP_SLEEP_WAKEUP_UART2, .last_wakeup_ext1 = UINT64_C(0x8000000012345678),
};
void solar_os_power_get_status(solar_os_power_status_t *out) { *out = power_value; }
esp_err_t solar_os_power_set_profile(solar_os_power_profile_t value) { assert(value == SOLAR_OS_POWER_PROFILE_BATTERY); return ESP_OK; }
esp_err_t solar_os_power_set_key_action(solar_os_power_key_action_t value) { assert(value == SOLAR_OS_POWER_KEY_ACTION_ENTER); return ESP_OK; }
esp_err_t solar_os_power_set_key_long_action(solar_os_power_key_action_t value) { assert(value == SOLAR_OS_POWER_KEY_ACTION_APP_EXIT); return ESP_OK; }
esp_err_t solar_os_power_set_idle_sleep_ms(uint32_t value) { assert(value == UINT32_MAX); return ESP_OK; }
esp_err_t solar_os_power_hold_automatic_light_sleep(uint32_t value) { assert(value == 3000); return ESP_OK; }
esp_err_t solar_os_power_off(void) { return ESP_ERR_NOT_SUPPORTED; }
void solar_os_power_note_activity(uint32_t value) { assert(value == 456); }
bool solar_os_power_should_idle_sleep(uint32_t value) { return value == 789; }
const char *solar_os_power_profile_name(solar_os_power_profile_t value)
{ assert(value == SOLAR_OS_POWER_PROFILE_LOWPOWER); return "lowpower"; }
const char *solar_os_power_key_action_name(solar_os_power_key_action_t value)
{ assert(value == SOLAR_OS_POWER_KEY_ACTION_SHUTDOWN); return "shutdown"; }
bool solar_os_power_parse_profile(const char *text, solar_os_power_profile_t *value)
{ if (strcmp(text, "battery")) return false; *value = SOLAR_OS_POWER_PROFILE_BATTERY; return true; }
bool solar_os_power_parse_key_action(const char *text, solar_os_power_key_action_t *value)
{ if (strcmp(text, "enter")) return false; *value = SOLAR_OS_POWER_KEY_ACTION_ENTER; return true; }
static esp_err_t shutdown_step(solar_os_shutdown_stage_t stage, void *user)
{ assert(stage == SOLAR_OS_SHUTDOWN_WAKE && user == &power_value); return ESP_ERR_TIMEOUT; }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GPIO
#include "solar_os_status_led.h"
static bool led_on;
esp_err_t solar_os_status_led_set(bool on) { led_on = on; return ESP_OK; }
esp_err_t solar_os_status_led_get(bool *out) { if (!out) return ESP_ERR_INVALID_ARG; *out = led_on; return ESP_OK; }
esp_err_t solar_os_status_led_toggle(bool *out) { led_on = !led_on; if (out) *out = led_on; return ESP_OK; }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
#include "solar_os_battery.h"
/* The battery registry and provider bridge are exercised by battery_service_test. */
esp_err_t solar_os_battery_register_provider(const char *owner, const solar_os_battery_provider_t *provider)
{
    (void)owner;
    (void)provider;
    assert(false);
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t solar_os_battery_unregister_provider_owned(const char *owner, const void *user)
{
    (void)owner;
    (void)user;
    assert(false);
    return ESP_ERR_NOT_SUPPORTED;
}
bool solar_os_battery_has_provider(void) { return true; }
esp_err_t solar_os_battery_get_status(solar_os_battery_status_t *out)
{ *out = (solar_os_battery_status_t){.voltage_mv = 4137, .percent = 87, .percent_estimated = true, .adc_calibrated = true, .external_power = true, .charging = true, .charging_known = true, .external_power_known = true}; return ESP_OK; }
void solar_os_battery_get_config(solar_os_battery_config_t *out)
{ *out = (solar_os_battery_config_t){.capacity_mah = 5000, .min_voltage_mv = 3300, .max_voltage_mv = 4200, .runtime_min = 10000}; }
esp_err_t solar_os_battery_set_capacity_mah(uint32_t value) { assert(value == 5000); return ESP_OK; }
esp_err_t solar_os_battery_set_min_voltage_mv(uint16_t value) { assert(value == UINT16_MAX); return ESP_OK; }
esp_err_t solar_os_battery_set_max_voltage_mv(uint16_t value) { assert(value == 4200); return ESP_OK; }
esp_err_t solar_os_battery_set_runtime_min(uint32_t value) { assert(value == 10000); return ESP_OK; }
esp_err_t solar_os_battery_monitor_start(uint32_t value) { assert(value == 1000); return ESP_OK; }
void solar_os_battery_monitor_stop(void) {}
esp_err_t solar_os_battery_monitor_sample(uint32_t value) { assert(value == UINT32_MAX); return ESP_ERR_TIMEOUT; }
void solar_os_battery_monitor_get_status(solar_os_battery_monitor_status_t *out)
{
    *out = (solar_os_battery_monitor_status_t){.running = true, .interval_ms = 1000, .sample_count = 123, .last_sample_ms = UINT32_MAX,
        .last_voltage_mv = 4137, .last_percent = 87, .adc_calibrated = true, .trend = SOLAR_OS_BATTERY_TREND_CHARGING,
        .external_power = true, .slope_mvh = -23, .time_left_min = 999, .time_left_valid = true, .last_error = ESP_ERR_NO_MEM,
        .external_power_known = true, .short_window = {.valid = true, .span_ms = 1234, .voltage_drop_mvh = -30, .current_valid = true, .current_ma = -199},
        .long_window = {.valid = true, .span_ms = 9999, .voltage_drop_mvh = 100, .current_valid = true, .current_ma = 250},
        .time_left_seeded = true, .full_runtime_min = 4000};
}
const char *solar_os_battery_trend_name(solar_os_battery_trend_t value)
{ assert(value == SOLAR_OS_BATTERY_TREND_CHARGING); return "charging"; }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CHARGER
#include "solar_os_charger.h"
static uint16_t selected_current;
static esp_err_t charger_read(void *ctx, solar_os_charger_status_t *out)
{
    assert(ctx == &selected_current);
    *out = (solar_os_charger_status_t){.enabled = true, .input_present = true, .power_good = true,
        .state = SOLAR_OS_CHARGER_STATE_FAST_CHARGE, .input_current_limit_ma = 500, .charge_current_ma = selected_current,
        .charge_voltage_mv = 4200, .fault = 0xa5}; return ESP_OK;
}
static esp_err_t charger_enable(void *ctx, bool value) { assert(ctx == &selected_current && value); return ESP_OK; }
static esp_err_t charger_current(void *ctx, uint16_t value) { assert(ctx == &selected_current); selected_current = value; return ESP_OK; }
static esp_err_t charger_unsupported(void *ctx, uint16_t value) { assert(ctx == &selected_current && value); return ESP_ERR_NOT_SUPPORTED; }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_HAPTIC
#include "solar_os_haptic.h"
static uint16_t selected_effect;
static esp_err_t play_effect(void *ctx, uint16_t value) { assert(ctx == &selected_effect); selected_effect = value; return ESP_OK; }
static esp_err_t stop_effect(void *ctx) { assert(ctx == &selected_effect); selected_effect = 0; return ESP_OK; }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RTC
#include "solar_os.h"
#include "solar_os_native_handles.h"
#include "solar_os_rtc.h"

static const solar_os_app_t rtc_app = {.name = "rtc-test"};
static unsigned alarm_stops, countdown_stops;
static bool alarm_fails, countdown_stop_fails;
static uint8_t alarm_hour;
static uint32_t countdown_seconds, rtc_pending = SOLAR_OS_RTC_INTERRUPT_ALARM;

const solar_os_app_t *solar_os_application_descriptor(void) { return &rtc_app; }

static esp_err_t rtc_read_clock(void *user, solar_os_datetime_t *out)
{
    assert(!user);
    *out = (solar_os_datetime_t){.year = 2026, .month = 11, .day = 26, .weekday = 4};
    return ESP_OK;
}

static esp_err_t rtc_write_clock(void *user, const solar_os_datetime_t *value)
{
    assert(!user && value);
    return ESP_OK;
}

static esp_err_t rtc_test_set_alarm(void *user, const solar_os_rtc_alarm_t *value)
{
    assert(!user);
    if (alarm_fails) return ESP_ERR_TIMEOUT;
    alarm_hour = value->hour;
    return ESP_OK;
}

static esp_err_t rtc_test_disable_alarm(void *user)
{
    assert(!user);
    ++alarm_stops;
    alarm_hour = 0;
    return ESP_OK;
}

static esp_err_t rtc_test_set_countdown(void *user, uint32_t seconds, bool repeat)
{
    assert(!user && repeat);
    countdown_seconds = seconds;
    return ESP_OK;
}

static esp_err_t rtc_test_disable_countdown(void *user)
{
    assert(!user);
    if (countdown_stop_fails) return ESP_ERR_TIMEOUT;
    ++countdown_stops;
    countdown_seconds = 0;
    return ESP_OK;
}

static esp_err_t rtc_test_interrupt_status(void *user, uint32_t *out)
{
    assert(!user);
    *out = rtc_pending;
    return ESP_OK;
}

static esp_err_t rtc_test_interrupt_clear(void *user, uint32_t bits)
{
    assert(!user);
    rtc_pending &= ~bits;
    return ESP_OK;
}

static void verify_rtc(void)
{
    const solar_os_native_rtc_api_v1_t *api =
        solar_os_native_device_get_service("clock.rtc", 1, sizeof(*api));
    assert(api && !api->available());
    const solar_os_native_rtc_provider_api_v1_t *providers =
        solar_os_native_device_get_service("clock.rtc.provider", 1, sizeof(*providers));
    assert(providers && providers->provider_close(0) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    solar_os_rtc_info_t info;
    assert(api->info(&info) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !info.provider[0]);
    const solar_os_rtc_provider_t provider = {
        .get_utc_datetime = rtc_read_clock, .set_utc_datetime = rtc_write_clock,
        .set_alarm = rtc_test_set_alarm, .disable_alarm = rtc_test_disable_alarm,
        .set_countdown = rtc_test_set_countdown, .disable_countdown = rtc_test_disable_countdown,
        .get_interrupt_status = rtc_test_interrupt_status, .clear_interrupt_status = rtc_test_interrupt_clear,
        .interrupt_gpio = 17, .interrupt_active_level = 0,
    };
    solar_os_native_handle_t a, b;
    assert(api->lease_open("timer", &a) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !a);
    assert(!solar_os_rtc_register_provider("fixture-rtc", &provider));
    assert(api->available());
    assert(!api->info(&info) && !strcmp(info.provider, "fixture-rtc") && info.interrupt_gpio == 17);
    assert(info.capabilities == (SOLAR_OS_RTC_CAP_CALENDAR | SOLAR_OS_RTC_CAP_ALARM |
                                 SOLAR_OS_RTC_CAP_COUNTDOWN | SOLAR_OS_RTC_CAP_INTERRUPT_STATUS));
    assert(alarm_stops == 1 && countdown_stops == 1);

    assert(!api->lease_open("timer", &a) && !api->lease_open("timer", &b) && a != b);
    solar_os_rtc_timer_request_v1_t alarm = {
        .kind = SOLAR_OS_RTC_TIMER_ALARM,
        .configuration.alarm = {.match_fields = SOLAR_OS_RTC_ALARM_MATCH_HOUR, .hour = 7},
    };
    assert(!api->program(a, &alarm) && alarm_hour == 7);
    assert(api->program(b, &alarm) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->cancel(b, SOLAR_OS_RTC_TIMER_ALARM) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    alarm.configuration.alarm.hour = 8;
    alarm_fails = true;
    assert(api->program(a, &alarm) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && alarm_hour == 7);
    assert(api->program(b, &alarm) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    alarm_fails = false;
    assert(!api->lease_close(b) && alarm_hour == 7);

    const solar_os_rtc_timer_request_v1_t countdown = {
        .kind = SOLAR_OS_RTC_TIMER_COUNTDOWN, .configuration.countdown = {.seconds = 100, .repeat = true},
    };
    assert(!api->program(a, &countdown) && countdown_seconds == 100);
    countdown_stop_fails = true;
    assert(api->lease_close(a) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(!alarm_hour && countdown_seconds == 100 && alarm_stops == 2 && countdown_stops == 1);
    countdown_stop_fails = false;
    assert(!solar_os_native_handle_close_owner(&rtc_app));
    assert(!countdown_seconds && alarm_stops == 2 && countdown_stops == 2);
    assert(api->lease_close(a) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!api->info(&info) && !info.alarm_owner[0] && !info.countdown_owner[0]);

    uint32_t pending;
    rtc_pending = SOLAR_OS_RTC_INTERRUPT_COUNTDOWN;
    assert(!api->interrupt_status(&pending) && pending == SOLAR_OS_RTC_INTERRUPT_COUNTDOWN);
    assert(!api->interrupt_clear(pending));
    assert(!api->interrupt_status(&pending) && !pending);
    assert(!solar_os_rtc_unregister_provider("fixture-rtc") && !api->available());
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_SENSORS || SOLAR_OS_PACKAGE_SERVICE_IMU
#include "solar_os_stream.h"

static unsigned sensor_streams;
esp_err_t solar_os_stream_register(const solar_os_stream_driver_t *driver)
{
    assert(driver && driver->info.id[0]);
    ++sensor_streams;
    return ESP_OK;
}
esp_err_t solar_os_stream_unregister(const char *id)
{
    assert(id && *id && sensor_streams);
    --sensor_streams;
    return ESP_OK;
}
esp_err_t solar_os_stream_register_many(const solar_os_stream_driver_t *drivers, size_t count)
{
    for (size_t i = 0; i < count; ++i) assert(!solar_os_stream_register(&drivers[i]));
    return ESP_OK;
}
esp_err_t solar_os_stream_unregister_many_owned(const char *const *ids, const void *const *users, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        assert(users[i]);
        assert(!solar_os_stream_unregister(ids[i]));
    }
    return ESP_OK;
}

#endif

#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
#include "solar_os_sensors.h"
static esp_err_t read_temperature(void *user, float *temperature)
{
    *temperature = *(const float *)user;
    return ESP_OK;
}
static esp_err_t read_humidity(void *user, float *humidity)
{
    *humidity = *(const float *)user;
    return ESP_OK;
}

typedef struct {
    const solar_os_native_sensors_api_v1_t *api;
    solar_os_native_handle_t provider;
} environment_provider_t;
static int portable_temperature(void *user, float *temperature)
{
    const environment_provider_t *test = user;
    assert(test->api->provider_close(test->provider) == SOLAR_OS_NATIVE_ERROR_BUSY);
    *temperature = 18.0F;
    return SOLAR_OS_NATIVE_OK;
}

static void verify_environment(const solar_os_native_sensors_api_v1_t *api)
{
    float temperature = -12.5F, humidity = 66.0F;
    const solar_os_sensors_ops_t temp_ops = {.read_temperature = read_temperature};
    const solar_os_sensors_ops_t humid_ops = {.read_humidity = read_humidity};
    const solar_os_sensors_registration_t temp = {"temp", "host", &temp_ops, &temperature};
    const solar_os_sensors_registration_t humid = {"humid", "host", &humid_ops, &humidity};
    assert(!solar_os_sensors_register(&temp) && !solar_os_sensors_register(&humid));
    const uint32_t kind = SOLAR_OS_NATIVE_SENSOR_ENVIRONMENT;
    const uint32_t both = SOLAR_OS_SENSOR_CAP_TEMPERATURE | SOLAR_OS_SENSOR_CAP_HUMIDITY;
    solar_os_native_sensor_info_v1_t info;
    assert(api->count(kind) == 2 && !api->info(kind, 0, &info));
    assert(!strcmp(info.name, "temp") && !strcmp(info.driver, "host"));
    assert(info.capabilities == SOLAR_OS_SENSOR_CAP_TEMPERATURE && !info.state);
    assert(api->info(kind, 2, &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    solar_os_native_environment_v1_t sample;
    assert(!api->environment(NULL, both, &sample));
    assert(sample.temperature_c == temperature && sample.humidity_percent == humidity);
    assert(!api->environment("humid", SOLAR_OS_SENSOR_CAP_HUMIDITY, &sample));
    assert(sample.temperature_c == 0.0F && sample.humidity_percent == humidity);
    assert(api->environment("temp", 0, &sample) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->environment("temp", both, &sample) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(!solar_os_sensors_unregister(temp.name) && !solar_os_sensors_unregister(humid.name));
    assert(!api->count(kind) && !sensor_streams);
    assert(!solar_os_sensors_init());
    environment_provider_t provider = {.api = api};
    char provider_name[] = "portable-temp";
    solar_os_native_sensor_provider_v1_t descriptor = {.name = provider_name, .driver = "host",
        .kind = kind, .library_lease = 123, .user = &provider,
        .ops.environment.temperature = portable_temperature};
    assert(!api->provider_register(&descriptor, &provider.provider) && sensor_provider_pins == 1);
    solar_os_native_handle_t duplicate;
    assert(api->provider_register(&descriptor, &duplicate) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!duplicate && sensor_provider_pins == 1 && sensor_streams == 1);
    provider_name[0] = 'x';
    descriptor.ops.environment.temperature = NULL;
    assert(!api->environment("portable-temp", SOLAR_OS_SENSOR_CAP_TEMPERATURE, &sample));
    assert(sample.temperature_c == 18.0F && !sample.humidity_percent);
    assert(api->provider_report(provider.provider, SOLAR_OS_NATIVE_GNSS_POWER_CONTROL, false) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(!api->provider_close(provider.provider) && !sensor_provider_pins && !sensor_streams);

}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_IMU
#include "solar_os_imu.h"
static esp_err_t read_imu(void *user, uint32_t timeout_ms, solar_os_imu_sample_t *sample)
{
    assert(!user && timeout_ms == 135);
    assert(solar_os_imu_unregister("imu-test") == ESP_ERR_INVALID_STATE);
    *sample = (solar_os_imu_sample_t){.timestamp_us = UINT64_C(7654321000),
        .valid = SOLAR_OS_IMU_CAP_ACCELERATION, .acceleration_m_s2 = {0.0F, 4.5F, 9.8F}};
    return ESP_OK;
}

static void verify_imu(const solar_os_native_sensors_api_v1_t *api)
{
    const solar_os_imu_ops_t ops = {.read_sample = read_imu};
    const solar_os_imu_registration_t reg = {"imu-test", "host", SOLAR_OS_IMU_CAP_ACCELERATION, &ops, NULL};
    assert(!solar_os_imu_register(&reg));
    const uint32_t kind = SOLAR_OS_NATIVE_SENSOR_IMU;
    solar_os_native_sensor_info_v1_t info;
    assert(api->count(kind) == 1 && !api->info(kind, 0, &info));
    assert(info.capabilities == SOLAR_OS_IMU_CAP_ACCELERATION && !info.state);
    solar_os_native_imu_sample_v1_t sample;
    assert(!api->imu("imu-test", 135, &sample));
    assert(sample.valid == info.capabilities && sample.acceleration_m_s2[1] == 4.5F);
    assert(sample.timestamp_us == UINT64_C(7654321000));
    assert(!solar_os_imu_unregister(reg.name) && !api->count(kind) && !sensor_streams);
    const solar_os_native_sensor_provider_v1_t descriptor = {.name = reg.name, .driver = reg.driver,
        .kind = kind, .library_lease = 123, .ops.imu = {SOLAR_OS_IMU_CAP_ACCELERATION, read_imu}};
    solar_os_native_handle_t provider;
    assert(!api->provider_register(&descriptor, &provider) && sensor_provider_pins == 1 && sensor_streams == 1);
    assert(!api->imu(reg.name, 135, &sample) && sample.timestamp_us == UINT64_C(7654321000));
    assert(!api->provider_close(provider) && !sensor_provider_pins && !sensor_streams);

}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_GNSS
#include "solar_os_gnss.h"
typedef struct { bool powered, enabled; } receiver_state_t;
static esp_err_t read_gnss(void *user, uint32_t timeout_ms, solar_os_gnss_fix_t *fix)
{
    receiver_state_t *state = user;
    assert(state->powered && state->enabled && timeout_ms == 50);
    assert(solar_os_gnss_unregister("receiver") == ESP_ERR_INVALID_STATE);
    *fix = (solar_os_gnss_fix_t){.valid = true, .satellites_valid = true,
        .satellites = 9, .latitude_deg_e7 = -500000};
    return ESP_OK;
}
static esp_err_t set_gnss_power(void *user, bool enabled)
{
    ((receiver_state_t *)user)->powered = enabled;
    return ESP_OK;
}
static esp_err_t set_gnss_enabled(void *user, bool enabled)
{
    ((receiver_state_t *)user)->enabled = enabled;
    return ESP_OK;
}

static void verify_gnss(const solar_os_native_sensors_api_v1_t *api)
{
    receiver_state_t state = {0};
    const solar_os_gnss_ops_t ops = {read_gnss, set_gnss_power, set_gnss_enabled};
    const solar_os_gnss_registration_t reg = {"receiver", "host", &ops, &state, false};
    assert(!solar_os_gnss_register(&reg));
    const uint32_t kind = SOLAR_OS_NATIVE_SENSOR_GNSS;
    const uint32_t power = SOLAR_OS_NATIVE_GNSS_POWER_CONTROL, enable = SOLAR_OS_NATIVE_GNSS_ENABLE_CONTROL;
    solar_os_native_sensor_info_v1_t info;
    assert(api->count(kind) == 1 && !api->info(kind, 0, &info));
    assert(info.capabilities == (power | enable) && !(info.state & SOLAR_OS_NATIVE_GNSS_POWERED));
    solar_os_native_gnss_status_v1_t status;
    assert(!api->gnss_status(reg.name, 50, &status) && !status.powered && !status.fix_available);
    assert(api->gnss_set(reg.name, power | enable, true) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->gnss_set(reg.name, power, true) && !api->gnss_set(reg.name, enable, true));
    assert(!api->info(kind, 0, &info) && info.state == (SOLAR_OS_NATIVE_GNSS_POWERED | SOLAR_OS_NATIVE_GNSS_ENABLED));
    solar_os_native_gnss_fix_v1_t fix;
    assert(!api->gnss_fix(reg.name, 50, &fix) && fix.satellites_valid && fix.satellites == 9);
    assert(!api->gnss_status(reg.name, 50, &status) && status.fix_available && status.fix.latitude_deg_e7 == -500000);
    assert(!solar_os_gnss_unregister(reg.name) && !api->count(kind));
    const solar_os_native_sensor_provider_v1_t descriptor = {.name = reg.name, .driver = reg.driver,
        .kind = kind, .library_lease = 123, .user = &state,
        .ops.gnss = {.read = read_gnss, .set_enabled = set_gnss_enabled, .powered = true}};
    solar_os_native_handle_t provider;
    assert(!api->provider_register(&descriptor, &provider) && sensor_provider_pins == 1);
    assert(!api->gnss_fix(reg.name, 50, &fix));
    assert(!api->provider_report(provider, enable, false) && state.enabled);
    assert(!api->info(kind, 0, &info) && !(info.state & SOLAR_OS_NATIVE_GNSS_ENABLED));
    assert(!solar_os_gnss_unregister(reg.name) && !solar_os_gnss_register(&reg));
    assert(api->provider_report(provider, power, true) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!api->provider_close(provider) && !sensor_provider_pins);
    assert(api->count(kind) == 1 && !solar_os_gnss_unregister(reg.name));

}
#endif

static void verify_sensors(void)
{
    const solar_os_native_sensors_api_v1_t *api =
        solar_os_native_device_get_service("sensors", 1, sizeof(*api));
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
    assert(api && !api->count(UINT32_MAX));
    solar_os_native_sensor_info_v1_t info;
    assert(api->info(UINT32_MAX, 0, &info) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
    verify_environment(api);
#else
    assert(!api->environment && !api->count(SOLAR_OS_NATIVE_SENSOR_ENVIRONMENT));
#endif
#if SOLAR_OS_PACKAGE_SERVICE_IMU
    verify_imu(api);
#else
    assert(!api->imu && !api->count(SOLAR_OS_NATIVE_SENSOR_IMU));
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GNSS
    verify_gnss(api);
#else
    assert(!api->gnss_fix && !api->gnss_status && !api->gnss_set);
#endif
#else
    assert(!api);
#endif
}

int main(void)
{
    verify_sensors();
    assert(!solar_os_native_device_get_service("battery", 2, 0));
    assert(!solar_os_native_device_get_service("battery", 1, UINT32_MAX));
#if SOLAR_OS_PACKAGE_SERVICE_RTC
    verify_rtc();
#else
    assert(!solar_os_native_device_get_service("clock.rtc", 1, 0));
#endif
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
    verify_identity();
    const solar_os_native_board_api_v1_t *board = solar_os_native_device_get_service("board", 1, sizeof(*board)); assert(board);
    solar_os_native_board_info_v1_t info; assert(!board->info(&info)); memset(board_id, 'x', sizeof(board_id) - 1);
    assert(!strcmp(info.id, "host") && info.psram_bytes == 8388608 && info.capabilities == (SOLAR_OS_NATIVE_BOARD_CAP_CAMERA | SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_I2S));
    assert(board->info(&info) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE); board_id[0] = 0;
    assert(board->has(SOLAR_OS_NATIVE_BOARD_CAP_CAMERA | SOLAR_OS_NATIVE_BOARD_CAP_GPIO));
    assert(!board->has(UINT64_C(1) << 63) && !board->capability_name(UINT64_C(1) << 63));
    assert(!strcmp(board->capability_name(SOLAR_OS_NATIVE_BOARD_CAP_CAMERA), "camera"));
    uint64_t bits; hardware_caps |= UINT64_C(1) << 63; assert(board->capabilities(&bits) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    char text[32]; assert(!board->capabilities_format(text, sizeof(text)) && !strcmp(text, "camera"));
    assert(board->capabilities_format(text, 1) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
    const solar_os_native_backlight_api_v1_t *backlight = solar_os_native_device_get_service("backlight", 1, sizeof(*backlight)); assert(backlight);
    solar_os_native_backlight_status_v1_t light; assert(!backlight->status(&light));
    assert(light.follow_display && light.suspended && light.percent == 73 && !light.effective_percent && light.providers == 2 && light.last_error == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(!backlight->set_brightness(100) && !backlight->display_changed(0));
    assert(backlight->set_brightness(256) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && backlight->display_changed(101) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!backlight->set_follow() && !backlight->set_suspended(true) && backlight->toggle() == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    const solar_os_native_power_api_v1_t *power = solar_os_native_device_get_service("power", 1, sizeof(*power)); assert(power);
    solar_os_native_power_status_v1_t status; assert(!power->status(&status));
    assert(status.profile == SOLAR_OS_NATIVE_POWER_LOWPOWER && status.effective_profile == SOLAR_OS_NATIVE_POWER_BALANCED && status.key_action == SOLAR_OS_NATIVE_POWER_KEY_SHUTDOWN);
    assert(status.last_wakeup_cause == SOLAR_OS_NATIVE_WAKE_UART2 && status.last_wakeup_ext1 == UINT64_C(0x8000000012345678));
    assert(status.pm_last_error == SOLAR_OS_NATIVE_ERROR_TIMEOUT && status.bt_sleep_last_error == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && status.idle_sleep_ms == UINT32_MAX);
    power_value.last_wakeup_cause = -1; assert(!power->status(&status) && status.last_wakeup_cause == SOLAR_OS_NATIVE_WAKE_UNKNOWN);
    power_value.profile = (solar_os_power_profile_t)99; assert(power->status(&status) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(power->set_profile(99) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !power->profile_name(99));
    assert(!power->set_profile(SOLAR_OS_NATIVE_POWER_BATTERY) && !power->set_key_action(SOLAR_OS_NATIVE_POWER_KEY_ENTER) && !power->set_key_long_action(SOLAR_OS_NATIVE_POWER_KEY_APP_EXIT));
    assert(!power->set_idle_sleep_ms(UINT32_MAX) && !power->hold_automatic_light_sleep(3000));
    power->note_activity(456); assert(power->should_idle_sleep(789) && power->off() == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    uint32_t value; assert(!power->parse_profile("battery", &value) && value == SOLAR_OS_NATIVE_POWER_BATTERY);
    assert(!power->parse_key_action("enter", &value) && value == SOLAR_OS_NATIVE_POWER_KEY_ENTER && power->parse_profile("invalid", &value) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!strcmp(power->profile_name(SOLAR_OS_NATIVE_POWER_LOWPOWER), "lowpower") && !strcmp(power->key_action_name(SOLAR_OS_NATIVE_POWER_KEY_SHUTDOWN), "shutdown"));
    solar_os_native_shutdown_status_v1_t shutdown; assert(!power->shutdown_status(&shutdown) && !shutdown.requested && !shutdown.error);
    assert(!power->request_shutdown() && power->request_shutdown() == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    solar_os_shutdown_poll(shutdown_step, &power_value, 500); assert(!power->shutdown_status(&shutdown));
    assert(shutdown.requested && shutdown.error == SOLAR_OS_NATIVE_ERROR_TIMEOUT && shutdown.stage == SOLAR_OS_NATIVE_SHUTDOWN_WAKE);
    solar_os_shutdown_poll(shutdown_step, &power_value, 600); assert(!strcmp(power->shutdown_stage_name(shutdown.stage), "wake source"));
#else
    assert(!solar_os_native_device_get_service("board", 1, 0) && !solar_os_native_device_get_service("power", 1, 0) && !solar_os_native_device_get_service("backlight", 1, 0));
    assert(!solar_os_native_device_get_service("device.identity", 1, 0));
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GPIO
    const solar_os_native_status_led_api_v1_t *led = solar_os_native_device_get_service("status_led", 1, sizeof(*led)); assert(led);
    bool on; assert(!led->set(true) && !led->get(&on) && on && !led->toggle(&on) && !on);
#else
    assert(!solar_os_native_device_get_service("status_led", 1, 0));
#endif
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
    const solar_os_native_battery_api_v1_t *battery = solar_os_native_device_get_service("battery", 1, sizeof(*battery)); assert(battery && battery->has_provider());
    solar_os_native_battery_status_v1_t charge; assert(!battery->status(&charge) && charge.voltage_mv == 4137 && charge.percent == 87 && charge.charging_known && charge.charging);
    solar_os_native_battery_config_v1_t config; assert(!battery->config(&config) && config.capacity_mah == 5000 && config.runtime_min == 10000);
    assert(!battery->set_capacity_mah(5000) && !battery->set_runtime_min(10000) && !battery->set_min_voltage_mv(UINT16_MAX) && !battery->set_max_voltage_mv(4200));
    assert(battery->set_min_voltage_mv(65536) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && battery->set_max_voltage_mv(UINT32_MAX) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    solar_os_native_battery_monitor_v1_t monitor; assert(!battery->monitor_status(&monitor));
    assert(monitor.running && monitor.last_sample_ms == UINT32_MAX && monitor.trend == SOLAR_OS_NATIVE_BATTERY_CHARGING && monitor.slope_mvh == -23);
    assert(monitor.short_window.current_ma == -199 && monitor.long_window.span_ms == 9999 && monitor.last_error == SOLAR_OS_NATIVE_ERROR_NO_MEMORY && monitor.time_left_seeded && monitor.full_runtime_min == 4000);
    assert(!battery->monitor_start(1000) && battery->monitor_sample(UINT32_MAX) == SOLAR_OS_NATIVE_ERROR_TIMEOUT); battery->monitor_stop();
    assert(!strcmp(battery->trend_name(SOLAR_OS_NATIVE_BATTERY_CHARGING), "charging") && !battery->trend_name(99));
#else
    assert(!solar_os_native_device_get_service("battery", 1, 0));
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CHARGER
    const solar_os_charger_ops_t ops = {.read_status = charger_read, .set_enabled = charger_enable, .set_charge_current = charger_current,
        .set_input_current_limit = charger_unsupported, .set_charge_voltage = charger_unsupported};
    solar_os_charger_registration_t registration = {.name = "charger", .driver = "host", .ctx = &selected_current, .ops = &ops,
        .input_current_limit_ma = {100, 500, 100}, .charge_current_ma = {0, 1000, 50}, .charge_voltage_mv = {4000, 4400, 20}};
    assert(!solar_os_charger_register(&registration));
    const solar_os_native_charger_api_v1_t *charger = solar_os_native_device_get_service("charger", 1, sizeof(*charger)); assert(charger && charger->count() == 1);
    solar_os_native_charger_info_v1_t device; assert(!charger->info(0, &device) && !strcmp(device.driver, "host") && device.charge_current_ma.maximum == 1000 && device.charge_current_ma.step == 50);
    assert(charger->set_charge_current("charger", 65536) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && charger->set_charge_current("charger", 251) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!charger->set_charge_current("charger", 250) && !charger->set_enabled("charger", true));
    assert(charger->set_input_current_limit("charger", 500) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    solar_os_native_charger_status_v1_t state; assert(!charger->status("charger", &state) && state.state == SOLAR_OS_NATIVE_CHARGER_FAST_CHARGE && state.fault == 0xa5 && state.charge_current_ma == 250);
    assert(!charger->state_name(99) && !strcmp(charger->state_name(SOLAR_OS_NATIVE_CHARGER_FAST_CHARGE), "fast-charge"));
    assert(!solar_os_charger_unregister("charger") && charger->status("charger", &state) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !charger->count());
#else
    assert(!solar_os_native_device_get_service("charger", 1, 0));
#endif
#if SOLAR_OS_PACKAGE_SERVICE_HAPTIC
    const solar_os_haptic_ops_t ops_haptic = {.play_effect = play_effect, .stop = stop_effect};
    solar_os_haptic_registration_t registration_haptic = {.name = "motor", .driver = "host", .effect_count = 123, .ops = &ops_haptic, .ctx = &selected_effect};
    assert(!solar_os_haptic_register(&registration_haptic));
    const solar_os_native_haptic_api_v1_t *haptic = solar_os_native_device_get_service("haptic", 1, sizeof(*haptic)); assert(haptic && haptic->count() == 1);
    solar_os_native_haptic_info_v1_t motor; assert(!haptic->info(0, &motor) && motor.effect_count == 123 && !strcmp(motor.name, "motor"));
    assert(haptic->play_effect("motor", UINT32_MAX) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && haptic->play_effect("motor", 124) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!haptic->play_effect("motor", 123) && selected_effect == 123 && !haptic->stop("motor") && !selected_effect);
    assert(!solar_os_haptic_unregister("motor") && !haptic->count());
#else
    assert(!solar_os_native_device_get_service("haptic", 1, 0));
#endif
    return 0;
}
