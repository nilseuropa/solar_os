#include "solar_os_native_device_providers.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_native.h"
#include "solar_os_memory.h"
#include "solar_os_config.h"
#include <string.h>
#if SOLAR_OS_PACKAGE_CORE_RUNTIME || SOLAR_OS_PACKAGE_SERVICE_HAPTIC || SOLAR_OS_PACKAGE_SERVICE_CHARGER
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_backlight.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_HAPTIC
#include "solar_os_haptic.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CHARGER
#include "solar_os_charger.h"
#endif
/* OS registries pin callbacks while invoking them. Unregister must finish (or
 * return busy) before this state and its code lease can be released. */
typedef struct {
    uint32_t kind, lease;
    solar_os_native_handle_t handle;
    bool registered;
    union {
        solar_os_native_backlight_provider_v1_t backlight;
        solar_os_native_haptic_provider_v1_t haptic;
        solar_os_native_charger_provider_v1_t charger;
    } callbacks;
    char name[20], driver[24];
} native_device_provider_t;
static bool provider_callback_enter(native_device_provider_t *state)
{ return solar_os_native_handle_acquire(state->handle, state->kind) != NULL; }
static esp_err_t provider_callback_leave(native_device_provider_t *state, bool held, int result)
{
    if (held) solar_os_native_handle_release(state->handle);
    return solar_os_native_callback_result(result);
}
static int provider_destroy(void *opaque)
{
    native_device_provider_t *state = opaque;
    if (state->registered) {
        esp_err_t error = ESP_ERR_NOT_SUPPORTED;
        switch (state->kind) {
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
        case SOLAR_OS_NATIVE_HANDLE_BACKLIGHT_PROVIDER:
            solar_os_backlight_unregister(state); error = ESP_OK; break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_HAPTIC
        case SOLAR_OS_NATIVE_HANDLE_HAPTIC_PROVIDER:
            error = solar_os_haptic_unregister_owned(state->name, state); break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CHARGER
        case SOLAR_OS_NATIVE_HANDLE_CHARGER_PROVIDER:
            error = solar_os_charger_unregister_owned(state->name, state); break;
#endif
        default: break;
        }
        if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
        state->registered = false;
    }
    if (state->lease) {
        int result = solar_os_native_library_release(state->lease);
        if (result && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
        state->lease = 0;
    }
    solar_os_memory_free(state); return SOLAR_OS_NATIVE_OK;
}
static int provider_create(uint32_t kind, uint32_t lease, const char *name, const char *driver,
    solar_os_native_handle_t *out, native_device_provider_t **state)
{
    *out = 0; *state = NULL;
    if (!lease || (name && (!*name || strlen(name) >= 20)) || (driver && strlen(driver) >= 24))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_device_provider_t *value = solar_os_memory_calloc(1, sizeof(*value), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.provider");
    if (!value) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    value->kind = kind;
    if (name) memcpy(value->name, name, strlen(name) + 1);
    if (driver) memcpy(value->driver, driver, strlen(driver) + 1);
    int result = solar_os_native_library_retain(lease, &value->lease);
    if (!result) result = solar_os_native_handle_create(kind, value, provider_destroy, out);
    if (result) { (void)provider_destroy(value); return result; }
    /* Registration can synchronously invoke a provider. Prevent recursive close
     * from reclaiming this state before the registry has published ownership. */
    value->handle = *out;
    (void)solar_os_native_handle_acquire(*out, kind);
    *state = value; return SOLAR_OS_NATIVE_OK;
}
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
static esp_err_t backlight_set(void *user, uint8_t percent)
{
    native_device_provider_t *state = user;
    bool held = provider_callback_enter(state);
    int result = state->callbacks.backlight.set(state->callbacks.backlight.user, percent);
    return provider_callback_leave(state, held, result);
}
int solar_os_native_backlight_provider_register(const solar_os_native_backlight_provider_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->set) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_device_provider_t *state;
    int result = provider_create(SOLAR_OS_NATIVE_HANDLE_BACKLIGHT_PROVIDER, in->library_lease, NULL, NULL, out, &state);
    if (result) return result;
    state->callbacks.backlight = *in;
    result = solar_os_native_result(solar_os_backlight_register(backlight_set, state));
    state->registered = !result;
    solar_os_native_handle_release(*out);
    if (result) { (void)solar_os_native_handle_close_kind(*out, state->kind); *out = 0; }
    return result;
}
int solar_os_native_backlight_provider_close(solar_os_native_handle_t handle)
{ return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_BACKLIGHT_PROVIDER); }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_HAPTIC
static esp_err_t haptic_play(void *user, uint16_t effect)
{
    native_device_provider_t *state = user;
    bool held = provider_callback_enter(state);
    int result = state->callbacks.haptic.play_effect(state->callbacks.haptic.user, effect);
    return provider_callback_leave(state, held, result);
}
static esp_err_t haptic_stop(void *user)
{
    native_device_provider_t *state = user;
    bool held = provider_callback_enter(state);
    int result = state->callbacks.haptic.stop(state->callbacks.haptic.user);
    return provider_callback_leave(state, held, result);
}
int solar_os_native_haptic_provider_register(const solar_os_native_haptic_provider_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->name || !in->driver || !in->play_effect || !in->stop || !in->effect_count || in->effect_count > UINT16_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_device_provider_t *state;
    int result = provider_create(SOLAR_OS_NATIVE_HANDLE_HAPTIC_PROVIDER, in->library_lease, in->name, in->driver, out, &state);
    if (result) return result;
    state->callbacks.haptic = *in;
    state->callbacks.haptic.name = state->name; state->callbacks.haptic.driver = state->driver;
    const solar_os_haptic_ops_t ops = {.play_effect = haptic_play, .stop = haptic_stop};
    const solar_os_haptic_registration_t registration = {.name = state->name, .driver = state->driver,
        .effect_count = (uint16_t)in->effect_count, .ops = &ops, .ctx = state};
    result = solar_os_native_result(solar_os_haptic_register(&registration));
    state->registered = !result;
    solar_os_native_handle_release(*out);
    if (result) { (void)solar_os_native_handle_close_kind(*out, state->kind); *out = 0; }
    return result;
}
int solar_os_native_haptic_provider_close(solar_os_native_handle_t handle)
{ return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_HAPTIC_PROVIDER); }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CHARGER
static bool charger_range_in(const solar_os_native_charger_range_v1_t *in, solar_os_charger_range_t *out)
{
    if (in->minimum > UINT16_MAX || in->maximum > UINT16_MAX || !in->step || in->step > UINT16_MAX) return false;
    *out = (solar_os_charger_range_t){.minimum = (uint16_t)in->minimum, .maximum = (uint16_t)in->maximum, .step = (uint16_t)in->step}; return true;
}
static esp_err_t charger_status(void *user, solar_os_charger_status_t *out)
{
    native_device_provider_t *state = user; solar_os_native_charger_status_v1_t value = {0};
    bool held = provider_callback_enter(state);
    int result = state->callbacks.charger.read_status(state->callbacks.charger.user, &value);
    esp_err_t error = provider_callback_leave(state, held, result);
    if (error) return error;
    memset(out, 0, sizeof(*out));
    switch (value.state) {
    case SOLAR_OS_NATIVE_CHARGER_UNKNOWN: out->state = SOLAR_OS_CHARGER_STATE_UNKNOWN; break;
    case SOLAR_OS_NATIVE_CHARGER_NOT_CHARGING: out->state = SOLAR_OS_CHARGER_STATE_NOT_CHARGING; break;
    case SOLAR_OS_NATIVE_CHARGER_PRECHARGE: out->state = SOLAR_OS_CHARGER_STATE_PRECHARGE; break;
    case SOLAR_OS_NATIVE_CHARGER_FAST_CHARGE: out->state = SOLAR_OS_CHARGER_STATE_FAST_CHARGE; break;
    case SOLAR_OS_NATIVE_CHARGER_DONE: out->state = SOLAR_OS_CHARGER_STATE_DONE; break;
    default: return ESP_ERR_INVALID_ARG;
    }
    if (value.input_current_limit_ma > UINT16_MAX || value.charge_current_ma > UINT16_MAX || value.charge_voltage_mv > UINT16_MAX || value.fault > UINT8_MAX)
        return ESP_ERR_INVALID_ARG;
    out->enabled = value.enabled != 0; out->input_present = value.input_present != 0; out->power_good = value.power_good != 0;
    out->input_current_limit_ma = (uint16_t)value.input_current_limit_ma;
    out->charge_current_ma = (uint16_t)value.charge_current_ma; out->charge_voltage_mv = (uint16_t)value.charge_voltage_mv;
    out->fault = (uint8_t)value.fault; return ESP_OK;
}
static esp_err_t charger_enabled(void *user, bool enabled)
{
    native_device_provider_t *state = user; bool held = provider_callback_enter(state);
    int result = state->callbacks.charger.set_enabled(state->callbacks.charger.user, enabled);
    return provider_callback_leave(state, held, result);
}
#define CHARGER_SETTER(name) \
static esp_err_t charger_##name(void *user, uint16_t value) \
{ native_device_provider_t *state = user; bool held = provider_callback_enter(state); \
  int result = state->callbacks.charger.name(state->callbacks.charger.user, value); \
  return provider_callback_leave(state, held, result); }
CHARGER_SETTER(set_input_current_limit)
CHARGER_SETTER(set_charge_current)
CHARGER_SETTER(set_charge_voltage)
#undef CHARGER_SETTER
int solar_os_native_charger_provider_register(const solar_os_native_charger_provider_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->name || !in->driver || !in->read_status || !in->set_enabled || !in->set_input_current_limit || !in->set_charge_current || !in->set_charge_voltage)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_charger_registration_t registration = {0};
    if (!charger_range_in(&in->input_current_limit_ma, &registration.input_current_limit_ma) ||
        !charger_range_in(&in->charge_current_ma, &registration.charge_current_ma) ||
        !charger_range_in(&in->charge_voltage_mv, &registration.charge_voltage_mv)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_device_provider_t *state;
    int result = provider_create(SOLAR_OS_NATIVE_HANDLE_CHARGER_PROVIDER, in->library_lease, in->name, in->driver, out, &state);
    if (result) return result;
    state->callbacks.charger = *in;
    state->callbacks.charger.name = state->name; state->callbacks.charger.driver = state->driver;
    const solar_os_charger_ops_t ops = {.read_status = charger_status, .set_enabled = charger_enabled,
        .set_input_current_limit = charger_set_input_current_limit, .set_charge_current = charger_set_charge_current, .set_charge_voltage = charger_set_charge_voltage};
    registration.name = state->name; registration.driver = state->driver; registration.ops = &ops; registration.ctx = state;
    result = solar_os_native_result(solar_os_charger_register(&registration));
    state->registered = !result;
    solar_os_native_handle_release(*out);
    if (result) { (void)solar_os_native_handle_close_kind(*out, state->kind); *out = 0; }
    return result;
}
int solar_os_native_charger_provider_close(solar_os_native_handle_t handle)
{ return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_CHARGER_PROVIDER); }
#endif
#endif
