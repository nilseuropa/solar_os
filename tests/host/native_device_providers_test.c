#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_device_providers.h"
#include "solar_os_native_handles.h"
#include "solar_os_haptic.h"
#include "solar_os_charger.h"
#include "solar_os_backlight.h"
#include "solar_os_time.h"
#include "solar_os_rtc.h"
#include "solar_os_memory.h"
#include "solar_os_log.h"
#include "nvs.h"
static unsigned allocations, retained, released, next_lease = 100;
static bool leases[256] = {[42] = true};
static bool allocation_failure, library_busy;
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{ assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag); if (allocation_failure) return NULL; void *p = calloc(count, size); if (p) ++allocations; return p; }
void solar_os_memory_free(void *p) { if (p) --allocations; free(p); }
int solar_os_native_library_retain(uint32_t source, uint32_t *out)
{ *out = 0; if (source >= 256 || !leases[source]) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND; *out = ++next_lease; leases[*out] = true; ++retained; return 0; }
int solar_os_native_library_release(uint32_t lease)
{
    if (lease >= 256 || !leases[lease]) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (library_busy && lease > 100) return SOLAR_OS_NATIVE_ERROR_BUSY;
    leases[lease] = false;
    if (lease > 100) ++released;
    return SOLAR_OS_NATIVE_OK;
}
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{ (void)name; (void)mode; *out = 1; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *name, uint8_t *out) { (void)handle; (void)name; (void)out; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *name, uint8_t value) { (void)handle; (void)name; (void)value; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "error"; }
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *format, ...)
{ (void)level; (void)tag; (void)format; return ESP_OK; }
static atomic_bool block_callback, callback_entered, allow_callback;
static uint32_t observed_effect;
static int play(void *user, uint32_t effect)
{
    assert(user == &observed_effect && retained > released); observed_effect = effect;
    atomic_store(&callback_entered, true);
    while (atomic_load(&block_callback) && !atomic_load(&allow_callback)) sched_yield();
    return SOLAR_OS_NATIVE_OK;
}
static int stop(void *user) { assert(user == &observed_effect); observed_effect = 0; return SOLAR_OS_NATIVE_OK; }
static void *invoke(void *unused) { (void)unused; assert(!solar_os_haptic_play_effect("motor", 3)); return NULL; }
static esp_err_t replacement_play(void *user, uint16_t effect) { (void)user; (void)effect; return ESP_OK; }
static esp_err_t replacement_stop(void *user) { (void)user; return ESP_OK; }
static solar_os_native_handle_t backlight_handle;
static unsigned backlight_calls;
static bool backlight_fail;
static int light(void *user, uint32_t percent)
{
    assert(user == &backlight_calls && percent <= 100 && retained > released); ++backlight_calls;
    /* register calls set before returning: recursive close must be busy. */
    if (backlight_calls == 1) assert(solar_os_native_backlight_provider_close(backlight_handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
    return backlight_fail ? SOLAR_OS_NATIVE_ERROR_FAILED : SOLAR_OS_NATIVE_OK;
}
static solar_os_native_charger_status_v1_t charger_status = {.enabled = 1, .input_present = 1, .power_good = 1,
    .state = SOLAR_OS_NATIVE_CHARGER_DONE, .input_current_limit_ma = 500, .charge_current_ma = 250, .charge_voltage_mv = 4200, .fault = 0xa5};
static int charger_read(void *user, solar_os_native_charger_status_v1_t *out)
{ assert(user == &charger_status && retained > released); *out = charger_status; return SOLAR_OS_NATIVE_OK; }
static int charger_enabled(void *user, bool enabled) { assert(user == &charger_status); charger_status.enabled = enabled; return SOLAR_OS_NATIVE_OK; }
static int charger_current(void *user, uint32_t value) { assert(user == &charger_status); charger_status.charge_current_ma = value; return SOLAR_OS_NATIVE_OK; }
static int destroy_dummy(void *state) { assert(state == &released); return SOLAR_OS_NATIVE_OK; }

static solar_os_native_datetime_v1_t clock_value = {
    .year = 2026, .month = 11, .day = 26, .weekday = 4, .clock_integrity = 1,
};

static int provider_read_clock(void *user, solar_os_native_datetime_v1_t *value)
{
    assert(user == &clock_value && retained > released);
    atomic_store(&callback_entered, true);
    while (atomic_load(&block_callback) && !atomic_load(&allow_callback)) sched_yield();
    *value = clock_value;
    return SOLAR_OS_NATIVE_OK;
}

static int provider_write_clock(void *user, const solar_os_native_datetime_v1_t *value)
{
    assert(user == &clock_value && retained > released);
    clock_value = *value;
    return SOLAR_OS_NATIVE_OK;
}

static void *invoke_clock(void *unused)
{
    (void)unused;
    solar_os_datetime_t value;
    assert(!solar_os_time_get_utc_datetime(&value) && value.year == 2026);
    return NULL;
}

static esp_err_t replacement_read_clock(void *user, solar_os_datetime_t *value)
{
    *value = *(const solar_os_datetime_t *)user;
    return ESP_OK;
}

static esp_err_t replacement_write_clock(void *user, const solar_os_datetime_t *value)
{
    *(solar_os_datetime_t *)user = *value;
    return ESP_OK;
}

static void test_clock_provider(void)
{
    const solar_os_native_clock_provider_v1_t clock = {
        .name = "fixture-clock", .library_lease = 42, .user = &clock_value,
        .get_utc = provider_read_clock, .set_utc = provider_write_clock,
    };
    solar_os_native_handle_t handle, duplicate;
    assert(!solar_os_native_clock_provider_register(&clock, &handle) && solar_os_time_has_provider());
    library_busy = true;
    assert(solar_os_native_clock_provider_register(&clock, &duplicate) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(duplicate && solar_os_time_has_provider());
    library_busy = false;
    assert(!solar_os_native_clock_provider_close(duplicate) && solar_os_time_has_provider());

    solar_os_datetime_t value;
    assert(!solar_os_time_get_utc_datetime(&value) && value.year == 2026);
    value.hour = 13;
    assert(!solar_os_time_set_utc_datetime(&value) && clock_value.hour == 13);
    value.day = 40;
    assert(solar_os_time_set_utc_datetime(&value) == ESP_ERR_INVALID_ARG && clock_value.day == 26);
    clock_value.month = 13;
    assert(solar_os_time_get_utc_datetime(&value) == ESP_ERR_INVALID_ARG);
    clock_value.month = 11;

    atomic_store(&callback_entered, false);
    atomic_store(&allow_callback, false);
    atomic_store(&block_callback, true);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, invoke_clock, NULL));
    while (!atomic_load(&callback_entered)) sched_yield();
    assert(solar_os_native_clock_provider_close(handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(solar_os_time_has_provider() && retained > released);
    atomic_store(&allow_callback, true);
    assert(!pthread_join(thread, NULL));

    /* Core replacement with the same public name leaves the old token stale. */
    assert(!solar_os_time_unregister_provider("fixture-clock"));
    solar_os_datetime_t replacement = {.year = 2040, .month = 1, .day = 1};
    const solar_os_time_provider_t next = {replacement_read_clock, replacement_write_clock, &replacement};
    assert(!solar_os_time_register_provider("fixture-clock", &next));
    assert(!solar_os_native_clock_provider_close(handle) && retained == released && !allocations);
    assert(!solar_os_time_get_utc_datetime(&value) && value.year == 2040);
    assert(!solar_os_time_unregister_provider("fixture-clock") && !solar_os_time_has_provider());
}

static solar_os_native_handle_t rtc_provider_handle;
static unsigned rtc_alarm_cancels, rtc_countdown_cancels;
static bool rtc_starting, rtc_cancel_fails;
static uint32_t rtc_irq = SOLAR_OS_RTC_INTERRUPT_ALARM;
static uint32_t rtc_alarm_hour, rtc_countdown_duration;

static int provider_program_rtc(void *user, const solar_os_rtc_timer_request_v1_t *request)
{
    assert(user == &clock_value && retained > released);
    if (request->kind == SOLAR_OS_RTC_TIMER_ALARM) rtc_alarm_hour = request->configuration.alarm.hour;
    else {
        assert(request->kind == SOLAR_OS_RTC_TIMER_COUNTDOWN);
        rtc_countdown_duration = request->configuration.countdown.seconds;
    }
    return SOLAR_OS_NATIVE_OK;
}

static int provider_cancel_rtc(void *user, solar_os_rtc_timer_kind_t kind)
{
    assert(user == &clock_value && retained > released);
    if (rtc_starting) {
        assert(!solar_os_time_has_provider() && !solar_os_rtc_has_provider());
        assert(solar_os_native_rtc_provider_close(rtc_provider_handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
    }
    if (kind == SOLAR_OS_RTC_TIMER_ALARM) {
        ++rtc_alarm_cancels;
        rtc_alarm_hour = 0;
    } else {
        assert(kind == SOLAR_OS_RTC_TIMER_COUNTDOWN);
        if (rtc_cancel_fails) return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
        ++rtc_countdown_cancels;
        rtc_countdown_duration = 0;
    }
    return SOLAR_OS_NATIVE_OK;
}

static int provider_rtc_status(void *user, uint32_t *pending)
{
    assert(user == &clock_value);
    *pending = rtc_irq;
    return SOLAR_OS_NATIVE_OK;
}

static int provider_rtc_clear(void *user, uint32_t pending)
{
    assert(user == &clock_value);
    rtc_irq &= ~pending;
    return SOLAR_OS_NATIVE_OK;
}

static void test_rtc_provider(void)
{
    solar_os_native_rtc_provider_v1_t descriptor = {
        .calendar = {
            .name = "rtc-fixture", .library_lease = 42, .user = &clock_value,
            .get_utc = provider_read_clock, .set_utc = provider_write_clock,
        },
        .capabilities = SOLAR_OS_RTC_CAP_CALENDAR | SOLAR_OS_RTC_CAP_ALARM |
                        SOLAR_OS_RTC_CAP_COUNTDOWN | SOLAR_OS_RTC_CAP_INTERRUPT_STATUS,
        .interrupt_gpio = 17, .interrupt_active_level = 0,
        .program_timer = provider_program_rtc,
        .cancel_timer = provider_cancel_rtc,
        .interrupt_status = provider_rtc_status,
        .interrupt_clear = provider_rtc_clear,
    };
    descriptor.capabilities |= UINT32_C(0x80000000);
    assert(solar_os_native_rtc_provider_register(&descriptor, &rtc_provider_handle) ==
           SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !rtc_provider_handle);
    descriptor.capabilities &= ~UINT32_C(0x80000000);
    rtc_starting = true;
    assert(!solar_os_native_rtc_provider_register(&descriptor, &rtc_provider_handle));
    rtc_starting = false;
    assert(!rtc_irq && rtc_alarm_cancels == 1 && rtc_countdown_cancels == 1);
    solar_os_rtc_info_t info;
    assert(!solar_os_rtc_get_info(&info) && info.interrupt_gpio == 17 &&
           info.capabilities == descriptor.capabilities);
    solar_os_datetime_t readout;
    assert(!solar_os_time_get_utc_datetime(&readout) && readout.hour == 13);
    rtc_irq = SOLAR_OS_RTC_INTERRUPT_ALARM;
    uint32_t pending;
    assert(!solar_os_rtc_get_interrupt_status(&pending) && pending == SOLAR_OS_RTC_INTERRUPT_ALARM);
    const solar_os_rtc_alarm_t alarm = {.match_fields = SOLAR_OS_RTC_ALARM_MATCH_HOUR, .hour = 7};
    assert(!solar_os_rtc_set_alarm_for("fixture", &alarm) && rtc_alarm_hour == 7);
    assert(!solar_os_rtc_set_countdown_for("fixture", 30, true) && rtc_countdown_duration == 30);

    rtc_cancel_fails = true;
    assert(solar_os_native_rtc_provider_close(rtc_provider_handle) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(solar_os_rtc_has_provider() && solar_os_time_has_provider() && retained > released);
    assert(!rtc_alarm_hour && rtc_countdown_duration == 30 && rtc_alarm_cancels == 2 &&
           rtc_countdown_cancels == 1);
    rtc_cancel_fails = false;
    assert(!solar_os_native_rtc_provider_close(rtc_provider_handle));
    assert(!solar_os_rtc_has_provider() && !solar_os_time_has_provider());
    assert(!rtc_irq && !rtc_countdown_duration && rtc_countdown_cancels == 2 && rtc_alarm_cancels == 2);
    assert(retained == released && !allocations);
}

int main(void)
{
    char name[] = "motor", driver[] = "host";
    solar_os_native_haptic_provider_v1_t descriptor = {.name = name, .driver = driver, .effect_count = 123,
        .library_lease = 42, .user = &observed_effect, .play_effect = play, .stop = stop};
    solar_os_native_handle_t token;
    assert(!solar_os_native_haptic_provider_register(&descriptor, &token) && token && retained == 1 && !released);
    memset(name, 'x', sizeof(name) - 1); memset(driver, 'y', sizeof(driver) - 1); memset(&descriptor, 0, sizeof(descriptor));
    assert(!solar_os_native_library_release(42)); /* The independent provider lease remains. */
    solar_os_haptic_info_t info; assert(solar_os_haptic_get(0, &info) && !strcmp(info.name, "motor") && !strcmp(info.driver, "host"));
    atomic_store(&block_callback, true); pthread_t thread; assert(!pthread_create(&thread, NULL, invoke, NULL));
    while (!atomic_load(&callback_entered)) sched_yield();
    assert(solar_os_native_haptic_provider_close(token) == SOLAR_OS_NATIVE_ERROR_BUSY && !released && allocations == 1);
    assert(solar_os_native_charger_provider_close(token) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    atomic_store(&allow_callback, true); pthread_join(thread, NULL); assert(observed_effect == 3);
    /* A legacy caller revokes the provider and installs a same-name replacement.
     * Cleanup of the stale native token must leave that replacement untouched. */
    assert(!solar_os_haptic_unregister("motor"));
    const solar_os_haptic_ops_t replacement_ops = {.play_effect = replacement_play, .stop = replacement_stop};
    const solar_os_haptic_registration_t replacement = {.name = "motor", .driver = "other", .effect_count = 5, .ops = &replacement_ops, .ctx = &released};
    assert(!solar_os_haptic_register(&replacement));
    assert(!solar_os_native_haptic_provider_close(token) && retained == released && !allocations && solar_os_haptic_count() == 1);
    assert(solar_os_native_haptic_provider_close(token) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!solar_os_haptic_unregister("motor"));
    leases[42] = true;
    solar_os_native_charger_provider_v1_t charger = {.name = "charger", .driver = "host", .library_lease = 42, .user = &charger_status,
        .input_current_limit_ma = {100, 500, 100}, .charge_current_ma = {0, 1000, 50}, .charge_voltage_mv = {4000, 4400, 20},
        .read_status = charger_read, .set_enabled = charger_enabled, .set_input_current_limit = charger_current,
        .set_charge_current = charger_current, .set_charge_voltage = charger_current};
    assert(!solar_os_native_charger_provider_register(&charger, &token));
    solar_os_charger_status_t value; assert(!solar_os_charger_read_status("charger", &value) && value.state == SOLAR_OS_CHARGER_STATE_DONE && value.fault == 0xa5);
    charger_status.charge_current_ma = 65536; assert(solar_os_charger_read_status("charger", &value) == ESP_ERR_INVALID_ARG);
    charger_status.charge_current_ma = 250; charger_status.state = 99; assert(solar_os_charger_read_status("charger", &value) == ESP_ERR_INVALID_ARG);
    charger_status.state = SOLAR_OS_NATIVE_CHARGER_DONE;
    assert(!solar_os_charger_set_charge_current("charger", 500) && charger_status.charge_current_ma == 500);
    assert(!solar_os_native_charger_provider_close(token) && !allocations && retained == released);
    charger.charge_voltage_mv.maximum = 65536; assert(solar_os_native_charger_provider_register(&charger, &token) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !token);
    charger.charge_voltage_mv.maximum = 4400; charger.library_lease = 0; assert(solar_os_native_charger_provider_register(&charger, &token) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    charger.library_lease = 42; allocation_failure = true; assert(solar_os_native_charger_provider_register(&charger, &token) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !token); allocation_failure = false;
    const solar_os_native_backlight_provider_v1_t backlight = {.library_lease = 42, .user = &backlight_calls, .set = light};
    assert(!solar_os_native_backlight_provider_register(&backlight, &backlight_handle) && backlight_calls == 1);
    assert(!solar_os_native_backlight_provider_close(backlight_handle) && backlight_calls == 2 && !allocations && retained == released);
    backlight_fail = true;
    assert(solar_os_native_backlight_provider_register(&backlight, &backlight_handle) == SOLAR_OS_NATIVE_ERROR_FAILED && !backlight_handle);
    assert(!solar_os_native_handle_count() && !allocations && retained == released);
    test_clock_provider();
    test_rtc_provider();
    /* Registering with a full pool leaves the library lease untouched. */
    const unsigned leases_before_exhaustion = retained;
    solar_os_native_handle_t *occupied = NULL;
    size_t count = 0;
    for (;;) {
        solar_os_native_handle_t next;
        int code = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_GPIO, &released, destroy_dummy, &next);
        if (code) { assert(code == SOLAR_OS_NATIVE_ERROR_NO_MEMORY); break; }
        solar_os_native_handle_t *larger = realloc(occupied, (count + 1) * sizeof(*occupied));
        assert(larger); occupied = larger; occupied[count++] = next;
    }
    assert(solar_os_native_charger_provider_register(&charger, &token) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !token);
    assert(retained == leases_before_exhaustion && retained == released &&
           !allocations && solar_os_native_handle_count() == count);
    for (size_t i = 0; i < count; ++i) assert(!solar_os_native_handle_close(occupied[i]));
    free(occupied);
    assert(!solar_os_native_handle_count());
    return 0;
}
