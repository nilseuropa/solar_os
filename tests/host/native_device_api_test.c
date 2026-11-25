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
int solar_os_native_library_retain(uint32_t source, uint32_t *out)
{ (void)source; *out = 0; return SOLAR_OS_NATIVE_ERROR_NOT_FOUND; }
int solar_os_native_library_release(uint32_t lease) { (void)lease; return SOLAR_OS_NATIVE_ERROR_NOT_FOUND; }
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_board_caps.h"
#include "solar_os_backlight.h"
#include "solar_os_power.h"
#include "solar_os_shutdown.h"
#include "esp_sleep.h"
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
int main(void)
{
    assert(!solar_os_native_device_get_service("battery", 2, 0));
    assert(!solar_os_native_device_get_service("battery", 1, UINT32_MAX));
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
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
