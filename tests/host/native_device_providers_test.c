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
#include "solar_os_memory.h"
#include "solar_os_log.h"
#include "nvs.h"
static unsigned allocations, retained, released, next_lease = 100;
static bool leases[256] = {[42] = true};
static bool allocation_failure;
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{ assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag); if (allocation_failure) return NULL; void *p = calloc(count, size); if (p) ++allocations; return p; }
void solar_os_memory_free(void *p) { if (p) --allocations; free(p); }
int solar_os_native_library_retain(uint32_t source, uint32_t *out)
{ *out = 0; if (source >= 256 || !leases[source]) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND; *out = ++next_lease; leases[*out] = true; ++retained; return 0; }
int solar_os_native_library_release(uint32_t lease)
{ if (lease >= 256 || !leases[lease]) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND; leases[lease] = false; if (lease > 100) ++released; return 0; }
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
    /* Pool exhaustion happens after retaining the code lease. Construction
     * rollback must return that lease and leave the existing handles intact. */
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
    assert(retained == released && !allocations && solar_os_native_handle_count() == count);
    for (size_t i = 0; i < count; ++i) assert(!solar_os_native_handle_close(occupied[i]));
    free(occupied);
    assert(!solar_os_native_handle_count());
    return 0;
}
