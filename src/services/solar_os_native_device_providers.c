#include "solar_os_native_device_providers.h"
#include "solar_os_native_battery_provider.h"
#include "solar_os_native_pwm_provider.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_native.h"
#include "solar_os_memory.h"
#include "solar_os_config.h"
#include <stdatomic.h>
#include <string.h>
#if SOLAR_OS_PACKAGE_CORE_RUNTIME || SOLAR_OS_PACKAGE_SERVICE_HAPTIC || \
    SOLAR_OS_PACKAGE_SERVICE_CHARGER || SOLAR_OS_PACKAGE_SERVICE_RTC || SOLAR_OS_PACKAGE_SERVICE_RADIO || \
    SOLAR_OS_PACKAGE_SERVICE_EXPANSION || SOLAR_OS_PACKAGE_SERVICE_SENSORS || SOLAR_OS_PACKAGE_SERVICE_IMU || \
    SOLAR_OS_PACKAGE_SERVICE_GNSS || SOLAR_OS_PACKAGE_SERVICE_CAMERA || SOLAR_OS_PACKAGE_SERVICE_NFC || \
    SOLAR_OS_PACKAGE_SERVICE_MOTOR || SOLAR_OS_PACKAGE_SERVICE_ENCODER || SOLAR_OS_PACKAGE_SERVICE_BATTERY || \
    SOLAR_OS_PACKAGE_SERVICE_PWM
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_backlight.h"
#endif
#if SOLAR_OS_PACKAGE_CORE_RUNTIME || SOLAR_OS_PACKAGE_SERVICE_RTC
#include "solar_os_native_clock_values.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RTC
#include "solar_os_rtc.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RADIO
#include "solar_os_radio.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_HAPTIC
#include "solar_os_haptic.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CHARGER
#include "solar_os_charger.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
#include "solar_os_gpio_controller.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
#include "solar_os_sensors.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_IMU
#include "solar_os_imu.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GNSS
#include "solar_os_gnss.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CAMERA
#include "solar_os_camera_stream.h"
#include "solar_os_stream.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_NFC
#include "solar_os_nfc.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_MOTOR
#include "solar_os_motor.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ENCODER
#include "solar_os_encoder.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
#include "solar_os_battery.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_PWM
#include "solar_os_pwm.h"
#endif
/* OS registries pin callbacks while invoking them. Unregister must finish (or
 * return busy) before this state and its code lease can be released. */
typedef struct {
    uint32_t kind, lease;
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    void *driver_pin;
#endif
    solar_os_native_handle_t handle;
    bool registered;
    atomic_bool opening;
    union {
        solar_os_native_backlight_provider_v1_t backlight;
        solar_os_native_clock_provider_v1_t clock;
#if SOLAR_OS_PACKAGE_SERVICE_RTC
        solar_os_native_rtc_provider_v1_t rtc;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RADIO
        solar_os_native_radio_callbacks_v1_t radio;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
        solar_os_native_gpio_controller_provider_v1_t gpio;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS || SOLAR_OS_PACKAGE_SERVICE_IMU || SOLAR_OS_PACKAGE_SERVICE_GNSS
        solar_os_native_sensor_provider_v1_t sensor;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CAMERA
        struct {
            solar_os_native_camera_provider_v1_t descriptor;
            solar_os_camera_backend_ops_t ops;
            solar_os_camera_config_t config;
            char stream[SOLAR_OS_STREAM_DEVICE_MAX];
        } camera;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_NFC
        solar_os_native_nfc_provider_v1_t nfc;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_MOTOR
        solar_os_native_motor_provider_v1_t motor;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ENCODER
        solar_os_native_encoder_provider_v1_t encoder;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
        solar_os_native_battery_provider_v1_t battery;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_PWM
        solar_os_native_pwm_provider_callbacks_v1_t pwm;
#endif
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
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS || SOLAR_OS_PACKAGE_SERVICE_IMU || SOLAR_OS_PACKAGE_SERVICE_GNSS
static esp_err_t provider_sensor_unregister(native_device_provider_t *state)
{
    switch (state->callbacks.sensor.kind) {
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
    case SOLAR_OS_NATIVE_SENSOR_ENVIRONMENT: return solar_os_sensors_unregister_owned(state->name, state);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_IMU
    case SOLAR_OS_NATIVE_SENSOR_IMU: return solar_os_imu_unregister_owned(state->name, state);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GNSS
    case SOLAR_OS_NATIVE_SENSOR_GNSS: return solar_os_gnss_unregister_owned(state->name, state);
#endif
    default: return ESP_ERR_NOT_SUPPORTED;
    }
}
#endif

static int provider_destroy(void *opaque)
{
    native_device_provider_t *state = opaque;
    if (atomic_load(&state->opening)) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (state->registered) {
        esp_err_t error = ESP_ERR_NOT_SUPPORTED;
        switch (state->kind) {
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
        case SOLAR_OS_NATIVE_HANDLE_BACKLIGHT_PROVIDER:
            solar_os_backlight_unregister(state); error = ESP_OK; break;
        case SOLAR_OS_NATIVE_HANDLE_CLOCK_PROVIDER:
            error = solar_os_time_unregister_provider_owned(state->name, state);
            if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
            break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RTC
        case SOLAR_OS_NATIVE_HANDLE_RTC_PROVIDER:
            error = solar_os_rtc_unregister_provider_owned(state->name, state);
            if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
            break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RADIO
        case SOLAR_OS_NATIVE_HANDLE_RADIO_PROVIDER:
            error = solar_os_radio_unregister_owned(state->name, state);
            if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
            break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_HAPTIC
        case SOLAR_OS_NATIVE_HANDLE_HAPTIC_PROVIDER:
            error = solar_os_haptic_unregister_owned(state->name, state); break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CHARGER
        case SOLAR_OS_NATIVE_HANDLE_CHARGER_PROVIDER:
            error = solar_os_charger_unregister_owned(state->name, state); break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
        case SOLAR_OS_NATIVE_HANDLE_GPIO_CONTROLLER_PROVIDER:
            error = solar_os_gpio_controller_unregister_owned(state->name, state);
            if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
            break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS || SOLAR_OS_PACKAGE_SERVICE_IMU || SOLAR_OS_PACKAGE_SERVICE_GNSS
        case SOLAR_OS_NATIVE_HANDLE_SENSOR_PROVIDER:
            error = provider_sensor_unregister(state);
            if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
            break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CAMERA
        case SOLAR_OS_NATIVE_HANDLE_CAMERA_PROVIDER:
            error = solar_os_camera_unregister_backend_owned(state->driver, state);
            if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
            break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_NFC
        case SOLAR_OS_NATIVE_HANDLE_NFC_PROVIDER:
            error = solar_os_nfc_unregister_owned(state->name, state);
            if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
            break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_MOTOR
        case SOLAR_OS_NATIVE_HANDLE_MOTOR_PROVIDER:
            error = solar_os_motor_unregister_owned(state->name, state);
            if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
            break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ENCODER
        case SOLAR_OS_NATIVE_HANDLE_ENCODER_PROVIDER:
            error = solar_os_encoder_unregister_owned(state->name, state);
            if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
            break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
        case SOLAR_OS_NATIVE_HANDLE_BATTERY_PROVIDER:
            error = solar_os_battery_unregister_provider_owned(state->driver, state);
            if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
            break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_PWM
        case SOLAR_OS_NATIVE_HANDLE_PWM_PROVIDER:
            error = solar_os_pwm_unregister_owned(state->name, state);
            if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
            break;
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
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (state->driver_pin) solar_os_native_driver_unpin(state->driver_pin);
#endif
    solar_os_memory_free(state); return SOLAR_OS_NATIVE_OK;
}
static int provider_finish(native_device_provider_t *state, solar_os_native_handle_t *out, int result)
{
    const uint32_t kind = state->kind;
    state->registered = result == SOLAR_OS_NATIVE_OK;
    atomic_store(&state->opening, false);
    if (result) {
        const int close_result = solar_os_native_handle_close_kind(*out, kind);
        if (close_result == SOLAR_OS_NATIVE_OK || close_result == SOLAR_OS_NATIVE_ERROR_NOT_FOUND)
            *out = 0;
    }
    return result;
}

static int provider_create(uint32_t kind, uint32_t lease, const char *name, const char *driver,
    solar_os_native_handle_t *out, native_device_provider_t **state)
{
    *out = 0;
    *state = NULL;
#if !SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (!lease) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#endif
    if ((name && (!*name || strlen(name) >= 20)) || (driver && strlen(driver) >= 24))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_device_provider_t *value = solar_os_memory_calloc(1, sizeof(*value),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.provider");
    if (!value) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    value->kind = kind;
    if (name) strcpy(value->name, name);
    if (driver) strcpy(value->driver, driver);
    atomic_init(&value->opening, true);
    int result = solar_os_native_handle_create(kind, value, provider_destroy, out);
    if (result) {
        solar_os_memory_free(value);
        return result;
    }
    value->handle = *out;
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (!lease) {
        value->driver_pin = solar_os_native_driver_pin();
        result = value->driver_pin ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    } else
#endif
    {
        result = solar_os_native_library_retain(lease, &value->lease);
    }
    if (result) return provider_finish(value, out, result);
    *state = value;
    return SOLAR_OS_NATIVE_OK;
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
    return provider_finish(state, out, result);
}
int solar_os_native_backlight_provider_close(solar_os_native_handle_t handle)
{ return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_BACKLIGHT_PROVIDER); }

#endif
#if SOLAR_OS_PACKAGE_CORE_RUNTIME || SOLAR_OS_PACKAGE_SERVICE_RTC
static const solar_os_native_clock_provider_v1_t *clock_callbacks(const native_device_provider_t *state)
{
#if SOLAR_OS_PACKAGE_SERVICE_RTC
    if (state->kind == SOLAR_OS_NATIVE_HANDLE_RTC_PROVIDER) return &state->callbacks.rtc.calendar;
#endif
    return &state->callbacks.clock;
}

static esp_err_t clock_provider_get(void *user, solar_os_datetime_t *out)
{
    native_device_provider_t *state = user;
    solar_os_native_datetime_v1_t value = {0};
    const bool held = provider_callback_enter(state);
    const solar_os_native_clock_provider_v1_t *callbacks = clock_callbacks(state);
    const int result = callbacks->get_utc(callbacks->user, &value);
    const esp_err_t error = provider_callback_leave(state, held, result);
    if (error != ESP_OK) return error;
    if (!solar_os_native_datetime_in(&value, out) || !solar_os_time_datetime_is_valid(out))
        return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t clock_provider_set(void *user, const solar_os_datetime_t *in)
{
    if (!solar_os_time_datetime_is_valid(in)) return ESP_ERR_INVALID_ARG;
    native_device_provider_t *state = user;
    solar_os_native_datetime_v1_t value;
    solar_os_native_datetime_out(in, &value);
    const bool held = provider_callback_enter(state);
    const solar_os_native_clock_provider_v1_t *callbacks = clock_callbacks(state);
    const int result = callbacks->set_utc(callbacks->user, &value);
    return provider_callback_leave(state, held, result);
}

#endif
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
int solar_os_native_clock_provider_register(const solar_os_native_clock_provider_v1_t *in,
                                             solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->name || !in->get_utc || !in->set_utc) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_device_provider_t *state;
    int result = provider_create(SOLAR_OS_NATIVE_HANDLE_CLOCK_PROVIDER, in->library_lease,
        in->name, NULL, out, &state);
    if (result) return result;
    state->callbacks.clock = *in;
    state->callbacks.clock.name = state->name;
    const solar_os_time_provider_t provider = {
        .get_utc_datetime = clock_provider_get, .set_utc_datetime = clock_provider_set, .user = state,
    };
    result = solar_os_native_result(solar_os_time_register_provider(state->name, &provider));
    return provider_finish(state, out, result);
}

int solar_os_native_clock_provider_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_CLOCK_PROVIDER);
}
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RTC
static esp_err_t rtc_program_callback(native_device_provider_t *state,
                                      const solar_os_rtc_timer_request_v1_t *request)
{
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.rtc.program_timer(state->callbacks.rtc.calendar.user, request);
    return provider_callback_leave(state, held, result);
}

static esp_err_t rtc_set_alarm(void *user, const solar_os_rtc_alarm_t *alarm)
{
    const solar_os_rtc_timer_request_v1_t request = {
        .kind = SOLAR_OS_RTC_TIMER_ALARM, .configuration.alarm = *alarm,
    };
    return rtc_program_callback(user, &request);
}

static esp_err_t rtc_set_countdown(void *user, uint32_t seconds, bool repeat)
{
    const solar_os_rtc_timer_request_v1_t request = {
        .kind = SOLAR_OS_RTC_TIMER_COUNTDOWN, .configuration.countdown = {seconds, repeat},
    };
    return rtc_program_callback(user, &request);
}

static esp_err_t rtc_cancel_callback(native_device_provider_t *state, solar_os_rtc_timer_kind_t kind)
{
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.rtc.cancel_timer(state->callbacks.rtc.calendar.user, kind);
    return provider_callback_leave(state, held, result);
}

static esp_err_t rtc_disable_alarm(void *user)
{
    return rtc_cancel_callback(user, SOLAR_OS_RTC_TIMER_ALARM);
}

static esp_err_t rtc_disable_countdown(void *user)
{
    return rtc_cancel_callback(user, SOLAR_OS_RTC_TIMER_COUNTDOWN);
}

static esp_err_t rtc_interrupt_status(void *user, uint32_t *pending)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.rtc.interrupt_status(state->callbacks.rtc.calendar.user, pending);
    const esp_err_t error = provider_callback_leave(state, held, result);
    if (error == ESP_OK && (*pending & ~(SOLAR_OS_RTC_INTERRUPT_ALARM | SOLAR_OS_RTC_INTERRUPT_COUNTDOWN)))
        return ESP_ERR_INVALID_ARG;
    return error;
}

static esp_err_t rtc_interrupt_clear(void *user, uint32_t pending)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.rtc.interrupt_clear(state->callbacks.rtc.calendar.user, pending);
    return provider_callback_leave(state, held, result);
}

int solar_os_native_rtc_provider_register(const solar_os_native_rtc_provider_v1_t *in,
                                           solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->calendar.name || !in->calendar.get_utc || !in->calendar.set_utc)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const uint32_t timers = SOLAR_OS_RTC_CAP_ALARM | SOLAR_OS_RTC_CAP_COUNTDOWN;
    const uint32_t supported = SOLAR_OS_RTC_CAP_CALENDAR | timers | SOLAR_OS_RTC_CAP_INTERRUPT_STATUS;
    const bool has_timers = (in->capabilities & timers) != 0;
    const bool has_interrupts = (in->capabilities & SOLAR_OS_RTC_CAP_INTERRUPT_STATUS) != 0;
    if ((in->capabilities & ~supported) || !(in->capabilities & SOLAR_OS_RTC_CAP_CALENDAR) ||
        has_timers != (in->program_timer != NULL) || has_timers != (in->cancel_timer != NULL) ||
        has_interrupts != (in->interrupt_status != NULL) || has_interrupts != (in->interrupt_clear != NULL))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    native_device_provider_t *state;
    int result = provider_create(SOLAR_OS_NATIVE_HANDLE_RTC_PROVIDER, in->calendar.library_lease,
        in->calendar.name, NULL, out, &state);
    if (result) return result;
    state->callbacks.rtc = *in;
    state->callbacks.rtc.calendar.name = state->name;
    const bool has_alarm = (in->capabilities & SOLAR_OS_RTC_CAP_ALARM) != 0;
    const bool has_countdown = (in->capabilities & SOLAR_OS_RTC_CAP_COUNTDOWN) != 0;
    const solar_os_rtc_provider_t provider = {
        .get_utc_datetime = clock_provider_get,
        .set_utc_datetime = clock_provider_set,
        .set_alarm = has_alarm ? rtc_set_alarm : NULL,
        .disable_alarm = has_alarm ? rtc_disable_alarm : NULL,
        .set_countdown = has_countdown ? rtc_set_countdown : NULL,
        .disable_countdown = has_countdown ? rtc_disable_countdown : NULL,
        .get_interrupt_status = has_interrupts ? rtc_interrupt_status : NULL,
        .clear_interrupt_status = has_interrupts ? rtc_interrupt_clear : NULL,
        .user = state,
        .interrupt_gpio = in->interrupt_gpio,
        .interrupt_active_level = in->interrupt_active_level,
    };
    result = solar_os_native_result(solar_os_rtc_register_provider(state->name, &provider));
    return provider_finish(state, out, result);
}

int solar_os_native_rtc_provider_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_RTC_PROVIDER);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_RADIO
static esp_err_t radio_provider_configure(void *opaque, const solar_os_radio_config_t *config)
{
    native_device_provider_t *state = opaque;
    const solar_os_native_radio_callbacks_v1_t *provider = &state->callbacks.radio;
    if (!provider->configure) return ESP_ERR_NOT_SUPPORTED;
    const bool held = provider_callback_enter(state);
    const int result = provider->configure(provider->user, config);
    return provider_callback_leave(state, held, result);
}

static esp_err_t radio_provider_set_state(void *opaque, solar_os_radio_state_t mode)
{
    native_device_provider_t *state = opaque;
    const solar_os_native_radio_callbacks_v1_t *provider = &state->callbacks.radio;
    if (!provider->set_state) return ESP_ERR_NOT_SUPPORTED;
    const bool held = provider_callback_enter(state);
    const int result = provider->set_state(provider->user, mode);
    return provider_callback_leave(state, held, result);
}

static esp_err_t radio_provider_get_status(void *opaque, solar_os_radio_status_t *status)
{
    native_device_provider_t *state = opaque;
    const solar_os_native_radio_callbacks_v1_t *provider = &state->callbacks.radio;
    if (!provider->get_status) return ESP_OK;
    const bool held = provider_callback_enter(state);
    const int result = provider->get_status(provider->user, status);
    return provider_callback_leave(state, held, result);
}

static esp_err_t radio_provider_send(void *opaque, const solar_os_radio_packet_t *packet, uint32_t timeout)
{
    native_device_provider_t *state = opaque;
    const solar_os_native_radio_callbacks_v1_t *provider = &state->callbacks.radio;
    if (!provider->send) return ESP_ERR_NOT_SUPPORTED;
    const bool held = provider_callback_enter(state);
    const int result = provider->send(provider->user, packet, timeout);
    return provider_callback_leave(state, held, result);
}

static esp_err_t radio_provider_send_stream(void *opaque, const uint8_t *bytes, size_t size, uint32_t timeout)
{
    native_device_provider_t *state = opaque;
    const solar_os_native_radio_callbacks_v1_t *provider = &state->callbacks.radio;
    if (!provider->send_stream) return ESP_ERR_NOT_SUPPORTED;
    const bool held = provider_callback_enter(state);
    const int result = provider->send_stream(provider->user, bytes, size, timeout);
    return provider_callback_leave(state, held, result);
}

static esp_err_t radio_provider_receive(void *opaque, solar_os_radio_packet_t *packet, uint32_t timeout)
{
    native_device_provider_t *state = opaque;
    const solar_os_native_radio_callbacks_v1_t *provider = &state->callbacks.radio;
    if (!provider->receive) return ESP_ERR_NOT_SUPPORTED;
    const bool held = provider_callback_enter(state);
    const int result = provider->receive(provider->user, packet, timeout);
    return provider_callback_leave(state, held, result);
}

static esp_err_t radio_provider_read_rssi(void *opaque, int16_t *dbm)
{
    native_device_provider_t *state = opaque;
    const solar_os_native_radio_callbacks_v1_t *provider = &state->callbacks.radio;
    if (!provider->read_rssi) return ESP_ERR_NOT_SUPPORTED;
    const bool held = provider_callback_enter(state);
    const int result = provider->read_rssi(provider->user, dbm);
    return provider_callback_leave(state, held, result);
}

static esp_err_t radio_provider_tune_rx(void *opaque, uint32_t frequency, uint32_t *settle_ms)
{
    native_device_provider_t *state = opaque;
    const solar_os_native_radio_callbacks_v1_t *provider = &state->callbacks.radio;
    if (!provider->tune_rx) return ESP_ERR_NOT_SUPPORTED;
    const bool held = provider_callback_enter(state);
    const int result = provider->tune_rx(provider->user, frequency, settle_ms);
    return provider_callback_leave(state, held, result);
}

static const solar_os_radio_ops_t radio_provider_ops = {
    .configure = radio_provider_configure,
    .set_state = radio_provider_set_state,
    .get_status = radio_provider_get_status,
    .send = radio_provider_send,
    .send_stream = radio_provider_send_stream,
    .receive = radio_provider_receive,
    .read_rssi = radio_provider_read_rssi,
    .tune_rx = radio_provider_tune_rx,
};

int solar_os_native_radio_provider_register(const solar_os_native_radio_provider_v1_t *in,
                                             solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->name || !in->driver || !*in->driver ||
        in->initial_state > SOLAR_OS_RADIO_STATE_TX ||
        strnlen(in->driver, SOLAR_OS_RADIO_DRIVER_MAX) >= SOLAR_OS_RADIO_DRIVER_MAX ||
        ((in->features & SOLAR_OS_RADIO_FEATURE_PACKET) && (!in->callbacks.send || !in->callbacks.receive)) ||
        ((in->features & SOLAR_OS_RADIO_FEATURE_CONTINUOUS_TX) && !in->callbacks.send_stream) ||
        ((in->features & SOLAR_OS_RADIO_FEATURE_LIVE_RSSI) && !in->callbacks.read_rssi) ||
        ((in->features & SOLAR_OS_RADIO_FEATURE_FAST_TUNE) && !in->callbacks.tune_rx))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    native_device_provider_t *state;
    int result = provider_create(SOLAR_OS_NATIVE_HANDLE_RADIO_PROVIDER, in->library_lease,
        in->name, in->driver, out, &state);
    if (result) return result;
    state->callbacks.radio = in->callbacks;
    const solar_os_radio_registration_t registration = {
        .name = state->name,
        .driver = state->driver,
        .summary = in->summary,
        .modulations = in->modulations,
        .features = in->features,
        .rx_gain_modes = in->rx_gain_modes,
        .max_packet_len = in->max_packet_len,
        .default_config = in->default_config,
        .initial_state = in->initial_state,
        .ops = &radio_provider_ops,
        .ctx = state,
    };
    result = solar_os_native_result(solar_os_radio_register(&registration));
    return provider_finish(state, out, result);
}

int solar_os_native_radio_provider_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_RADIO_PROVIDER);
}
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
    return provider_finish(state, out, result);
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
    return provider_finish(state, out, result);
}
int solar_os_native_charger_provider_close(solar_os_native_handle_t handle)
{ return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_CHARGER_PROVIDER); }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
static esp_err_t provider_gpio_configure(void *user, uint8_t line,
    solar_os_gpio_line_mode_t mode, solar_os_gpio_line_pull_t pull)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.gpio.configure(state->callbacks.gpio.user, line, mode, pull);
    return provider_callback_leave(state, held, result);
}

static esp_err_t provider_gpio_read(void *user, uint8_t line, bool *level)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.gpio.read(state->callbacks.gpio.user, line, level);
    return provider_callback_leave(state, held, result);
}

static esp_err_t provider_gpio_write(void *user, uint8_t line, bool level)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.gpio.write(state->callbacks.gpio.user, line, level);
    return provider_callback_leave(state, held, result);
}

static esp_err_t provider_gpio_acknowledge(void *user)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.gpio.acknowledge_interrupt(state->callbacks.gpio.user);
    return provider_callback_leave(state, held, result);
}

int solar_os_native_gpio_controller_register(const solar_os_native_gpio_controller_provider_v1_t *in,
    solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->name || !in->line_count || !in->configure || !in->read || !in->write ||
        strnlen(in->name, SOLAR_OS_GPIO_CONTROLLER_NAME_MAX) >= SOLAR_OS_GPIO_CONTROLLER_NAME_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_device_provider_t *state;
    int result = provider_create(SOLAR_OS_NATIVE_HANDLE_GPIO_CONTROLLER_PROVIDER,
        in->library_lease, in->name, NULL, out, &state);
    if (result) return result;
    state->callbacks.gpio = *in;
    static const solar_os_gpio_controller_ops_t basic_ops = {
        .configure = provider_gpio_configure, .read = provider_gpio_read, .write = provider_gpio_write};
    static const solar_os_gpio_controller_ops_t interrupt_ops = {
        .configure = provider_gpio_configure, .read = provider_gpio_read, .write = provider_gpio_write,
        .acknowledge_interrupt = provider_gpio_acknowledge};
    const solar_os_gpio_controller_registration_t registration = {.name = state->name,
        .line_count = in->line_count, .ctx = state,
        .ops = in->acknowledge_interrupt ? &interrupt_ops : &basic_ops};
    result = solar_os_native_result(solar_os_gpio_controller_register(&registration));
    return provider_finish(state, out, result);
}

int solar_os_native_gpio_controller_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_GPIO_CONTROLLER_PROVIDER);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_SENSORS || SOLAR_OS_PACKAGE_SERVICE_IMU || SOLAR_OS_PACKAGE_SERVICE_GNSS
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
static esp_err_t provider_environment_read(void *user, solar_os_environment_t *sample)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.sensor.ops.environment.read(state->callbacks.sensor.user, sample);
    return provider_callback_leave(state, held, result);
}
static esp_err_t provider_temperature(void *user, float *value)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.sensor.ops.environment.temperature(state->callbacks.sensor.user, value);
    return provider_callback_leave(state, held, result);
}
static esp_err_t provider_humidity(void *user, float *value)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.sensor.ops.environment.humidity(state->callbacks.sensor.user, value);
    return provider_callback_leave(state, held, result);
}
static esp_err_t provider_register_environment(native_device_provider_t *state)
{
    const solar_os_native_sensor_provider_v1_t *sensor = &state->callbacks.sensor;
    const solar_os_sensors_ops_t ops = {
        .read_environment = sensor->ops.environment.read ? provider_environment_read : NULL,
        .read_temperature = sensor->ops.environment.temperature ? provider_temperature : NULL,
        .read_humidity = sensor->ops.environment.humidity ? provider_humidity : NULL,
    };
    const solar_os_sensors_registration_t registration = {
        .name = state->name, .driver = state->driver, .ops = &ops, .ctx = state,
    };
    return solar_os_sensors_register(&registration);
}
#endif
#if SOLAR_OS_PACKAGE_SERVICE_IMU
static esp_err_t provider_imu_read(void *user, uint32_t timeout, solar_os_imu_sample_t *sample)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.sensor.ops.imu.read(state->callbacks.sensor.user, timeout, sample);
    return provider_callback_leave(state, held, result);
}
static esp_err_t provider_register_imu(native_device_provider_t *state)
{
    if (!state->callbacks.sensor.ops.imu.read) return ESP_ERR_INVALID_ARG;
    static const solar_os_imu_ops_t ops = {.read_sample = provider_imu_read};
    const solar_os_imu_registration_t registration = {.name = state->name, .driver = state->driver,
        .capabilities = state->callbacks.sensor.ops.imu.capabilities, .ops = &ops, .ctx = state};
    return solar_os_imu_register(&registration);
}
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GNSS
static esp_err_t provider_gnss_read(void *user, uint32_t timeout, solar_os_gnss_fix_t *fix)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.sensor.ops.gnss.read(state->callbacks.sensor.user, timeout, fix);
    return provider_callback_leave(state, held, result);
}
static esp_err_t provider_gnss_power(void *user, bool on)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.sensor.ops.gnss.set_power(state->callbacks.sensor.user, on);
    return provider_callback_leave(state, held, result);
}
static esp_err_t provider_gnss_enable(void *user, bool on)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.sensor.ops.gnss.set_enabled(state->callbacks.sensor.user, on);
    return provider_callback_leave(state, held, result);
}
static esp_err_t provider_register_gnss(native_device_provider_t *state)
{
    const solar_os_native_sensor_provider_v1_t *sensor = &state->callbacks.sensor;
    if (!sensor->ops.gnss.read) return ESP_ERR_INVALID_ARG;
    static const solar_os_gnss_ops_t ops[] = {
        {.read_fix = provider_gnss_read},
        {.read_fix = provider_gnss_read, .set_power = provider_gnss_power},
        {.read_fix = provider_gnss_read, .set_enabled = provider_gnss_enable},
        {.read_fix = provider_gnss_read, .set_power = provider_gnss_power, .set_enabled = provider_gnss_enable},
    };
    const unsigned methods = (sensor->ops.gnss.set_power ? 1U : 0U) | (sensor->ops.gnss.set_enabled ? 2U : 0U);
    const solar_os_gnss_registration_t registration = {.name = state->name, .driver = state->driver,
        .ops = &ops[methods], .ctx = state, .powered = sensor->ops.gnss.powered};
    return solar_os_gnss_register(&registration);
}
#endif

int solar_os_native_sensor_provider_register(const solar_os_native_sensor_provider_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->name || !in->driver || in->kind < SOLAR_OS_NATIVE_SENSOR_ENVIRONMENT || in->kind > SOLAR_OS_NATIVE_SENSOR_GNSS)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    esp_err_t (*register_sensor)(native_device_provider_t *) = NULL;
    switch (in->kind) {
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
    case SOLAR_OS_NATIVE_SENSOR_ENVIRONMENT: register_sensor = provider_register_environment; break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_IMU
    case SOLAR_OS_NATIVE_SENSOR_IMU: register_sensor = provider_register_imu; break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GNSS
    case SOLAR_OS_NATIVE_SENSOR_GNSS: register_sensor = provider_register_gnss; break;
#endif
    default: return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    }
    native_device_provider_t *state;
    const int result = provider_create(SOLAR_OS_NATIVE_HANDLE_SENSOR_PROVIDER, in->library_lease,
        in->name, in->driver, out, &state);
    if (result) return result;
    state->callbacks.sensor = *in;
    return provider_finish(state, out, solar_os_native_result(register_sensor(state)));
}
int solar_os_native_sensor_provider_close(solar_os_native_handle_t handle)
{ return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_SENSOR_PROVIDER); }

int solar_os_native_sensor_provider_report(solar_os_native_handle_t handle, uint32_t property, bool enabled)
{
    native_device_provider_t *state = solar_os_native_handle_acquire(handle, SOLAR_OS_NATIVE_HANDLE_SENSOR_PROVIDER);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    esp_err_t error = ESP_ERR_NOT_SUPPORTED;
#if SOLAR_OS_PACKAGE_SERVICE_GNSS
    if (state->callbacks.sensor.kind == SOLAR_OS_NATIVE_SENSOR_GNSS)
        error = solar_os_gnss_notify_state_owned(state->name, state, property, enabled);
#else
    (void)property;
    (void)enabled;
#endif
    solar_os_native_handle_release(handle);
    return solar_os_native_result(error);
}
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CAMERA
static esp_err_t publish_camera_stream(void *user)
{
    native_device_provider_t *state = user;
    return solar_os_camera_stream_register_owned(state->callbacks.camera.stream,
        &state->callbacks.camera.config, state);
}

static esp_err_t unpublish_camera_stream(void *user)
{
    native_device_provider_t *state = user;
    return solar_os_camera_stream_unregister_owned(state->callbacks.camera.stream, state);
}

static esp_err_t camera_provider_start(void *user, const solar_os_camera_config_t *config,
    solar_os_camera_orientation_t orientation, solar_os_camera_sensor_info_t *sensor)
{
    native_device_provider_t *state = user;
    const solar_os_native_camera_provider_v1_t *provider = &state->callbacks.camera.descriptor;
    const bool held = provider_callback_enter(state);
    return provider_callback_leave(state, held, provider->start(provider->user, config, orientation, sensor));
}

static esp_err_t camera_provider_stop(void *user)
{
    native_device_provider_t *state = user;
    const solar_os_native_camera_provider_v1_t *provider = &state->callbacks.camera.descriptor;
    const bool held = provider_callback_enter(state);
    return provider_callback_leave(state, held, provider->stop(provider->user));
}

static esp_err_t camera_provider_capture(void *user, solar_os_camera_backend_frame_t *frame)
{
    native_device_provider_t *state = user;
    const solar_os_native_camera_provider_v1_t *provider = &state->callbacks.camera.descriptor;
    const bool held = provider_callback_enter(state);
    return provider_callback_leave(state, held, provider->capture(provider->user, frame));
}

static void camera_provider_release(void *user, void *token)
{
    native_device_provider_t *state = user;
    const solar_os_native_camera_provider_v1_t *provider = &state->callbacks.camera.descriptor;
    const bool held = provider_callback_enter(state);
    provider->release(provider->user, token);
    if (held) solar_os_native_handle_release(state->handle);
}

static esp_err_t camera_provider_resolutions(void *user, solar_os_camera_resolution_t *out, size_t capacity, size_t *count)
{
    native_device_provider_t *state = user;
    const solar_os_native_camera_provider_v1_t *provider = &state->callbacks.camera.descriptor;
    const bool held = provider_callback_enter(state);
    return provider_callback_leave(state, held, provider->get_resolutions(provider->user, out, capacity, count));
}

static esp_err_t camera_provider_orientation(void *user, solar_os_camera_orientation_t orientation,
    solar_os_camera_orientation_t *applied)
{
    native_device_provider_t *state = user;
    const solar_os_native_camera_provider_v1_t *provider = &state->callbacks.camera.descriptor;
    const bool held = provider_callback_enter(state);
    return provider_callback_leave(state, held, provider->set_orientation(provider->user, orientation, applied));
}

static void camera_provider_invalidate(void *user)
{
    native_device_provider_t *state = user;
    const solar_os_native_camera_provider_v1_t *provider = &state->callbacks.camera.descriptor;
    const bool held = provider_callback_enter(state);
    provider->invalidate(provider->user);
    if (held) solar_os_native_handle_release(state->handle);
}

int solar_os_native_camera_provider_register(const solar_os_native_camera_provider_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->driver || !*in->driver || !in->start || !in->stop || !in->capture || !in->release ||
        (in->stream_name && (!*in->stream_name || strlen(in->stream_name) >= SOLAR_OS_STREAM_DEVICE_MAX)))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_device_provider_t *state;
    int result = provider_create(SOLAR_OS_NATIVE_HANDLE_CAMERA_PROVIDER, in->library_lease, NULL, in->driver, out, &state);
    if (result) return result;
    state->callbacks.camera.descriptor = *in;
    state->callbacks.camera.config = in->default_config ? *in->default_config : solar_os_camera_default_config();
    if (in->stream_name) strcpy(state->callbacks.camera.stream, in->stream_name);
    state->callbacks.camera.ops = (solar_os_camera_backend_ops_t){
        .start = camera_provider_start, .stop = camera_provider_stop,
        .capture = camera_provider_capture, .release = camera_provider_release,
        .set_orientation = in->set_orientation ? camera_provider_orientation : NULL,
        .get_resolutions = in->get_resolutions ? camera_provider_resolutions : NULL,
        .invalidate = in->invalidate ? camera_provider_invalidate : NULL,
        .publish = in->stream_name ? publish_camera_stream : NULL,
        .unpublish = in->stream_name ? unpublish_camera_stream : NULL,
    };
    const solar_os_camera_backend_t backend = {
        .driver = state->driver, .ops = &state->callbacks.camera.ops, .ctx = state,
        .default_config = &state->callbacks.camera.config,
        .processor = in->processor, .processor_source = in->processor_source,
    };
    result = solar_os_native_result(solar_os_camera_register_backend(&backend));
    return provider_finish(state, out, result);
}

int solar_os_native_camera_provider_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_CAMERA_PROVIDER);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_NFC
static esp_err_t nfc_provider_scan(void *user, uint32_t timeout_ms, solar_os_nfc_tag_t *tag)
{
    native_device_provider_t *state = user;
    const solar_os_native_nfc_provider_v1_t *provider = &state->callbacks.nfc;
    const bool held = provider_callback_enter(state);
    solar_os_nfc_tag_t read = {0};
    const int result = provider->scan(provider->user, timeout_ms, &read);
    const esp_err_t error = provider_callback_leave(state, held, result);
    if (error) return error;
    if (read.technology != SOLAR_OS_NFC_TECHNOLOGY_NFCA || !read.uid_len || read.uid_len > sizeof(read.uid))
        return ESP_ERR_INVALID_RESPONSE;
    *tag = read;
    return ESP_OK;
}

static esp_err_t nfc_provider_set_power(void *user, bool enabled)
{
    native_device_provider_t *state = user;
    const solar_os_native_nfc_provider_v1_t *provider = &state->callbacks.nfc;
    const bool held = provider_callback_enter(state);
    return provider_callback_leave(state, held, provider->set_power(provider->user, enabled));
}

int solar_os_native_nfc_provider_register(const solar_os_native_nfc_provider_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->name || !in->driver || !*in->driver || !in->scan)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_device_provider_t *state;
    const int result = provider_create(SOLAR_OS_NATIVE_HANDLE_NFC_PROVIDER, in->library_lease, in->name, in->driver, out, &state);
    if (result) return result;
    state->callbacks.nfc = *in;
    static const solar_os_nfc_ops_t scan_only = {.scan = nfc_provider_scan};
    static const solar_os_nfc_ops_t power_control = {.scan = nfc_provider_scan, .set_power = nfc_provider_set_power};
    const solar_os_nfc_registration_t registration = {.name = state->name, .driver = state->driver,
        .ops = in->set_power ? &power_control : &scan_only, .ctx = state, .powered = in->powered};
    return provider_finish(state, out, solar_os_native_result(solar_os_nfc_register(&registration)));
}

int solar_os_native_nfc_provider_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_NFC_PROVIDER);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_MOTOR || SOLAR_OS_PACKAGE_SERVICE_ENCODER
static esp_err_t motion_call(native_device_provider_t *state, int (*callback)(void *), void *user)
{
    const bool held = provider_callback_enter(state);
    return provider_callback_leave(state, held, callback(user));
}

static esp_err_t motion_shutdown(native_device_provider_t *state, int (*callback)(void *), void *user)
{
    const esp_err_t error = motion_call(state, callback, user);
    /* Registry NOT_FOUND means the registration is gone. A callback error
     * means it remains live and teardown can be retried. */
    return error == ESP_ERR_NOT_FOUND ? ESP_FAIL : error;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_MOTOR
static esp_err_t motor_status(void *user, solar_os_motor_status_t *status)
{
    native_device_provider_t *state = user;
    const solar_os_native_motor_provider_v1_t *provider = &state->callbacks.motor;
    const bool held = provider_callback_enter(state);
    return provider_callback_leave(state, held, provider->status(provider->user, status));
}

static esp_err_t motor_drive(void *user, float demand)
{
    native_device_provider_t *state = user;
    const solar_os_native_motor_provider_v1_t *provider = &state->callbacks.motor;
    const bool held = provider_callback_enter(state);
    return provider_callback_leave(state, held, provider->drive(provider->user, demand));
}

static esp_err_t motor_coast(void *user)
{
    native_device_provider_t *state = user;
    return motion_call(state, state->callbacks.motor.coast, state->callbacks.motor.user);
}

static esp_err_t motor_brake(void *user)
{
    native_device_provider_t *state = user;
    return motion_call(state, state->callbacks.motor.brake, state->callbacks.motor.user);
}

static esp_err_t motor_clear(void *user)
{
    native_device_provider_t *state = user;
    return motion_call(state, state->callbacks.motor.clear, state->callbacks.motor.user);
}

static esp_err_t motor_shutdown(void *user)
{
    native_device_provider_t *state = user;
    const solar_os_native_motor_provider_v1_t *provider = &state->callbacks.motor;
    return motion_shutdown(state, provider->shutdown ? provider->shutdown : provider->coast, provider->user);
}

int solar_os_native_motor_provider_register(const solar_os_native_motor_provider_v1_t *in,
                                           solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->name || !in->driver || !*in->driver || !in->status || !in->coast)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    native_device_provider_t *state;
    int result = provider_create(SOLAR_OS_NATIVE_HANDLE_MOTOR_PROVIDER, in->library_lease,
        in->name, in->driver, out, &state);
    if (result) return result;
    state->callbacks.motor = *in;
    state->callbacks.motor.name = state->name;
    state->callbacks.motor.driver = state->driver;
    const solar_os_motor_ops_t ops = {
        .status = motor_status, .drive = in->drive ? motor_drive : NULL, .coast = motor_coast,
        .brake = in->brake ? motor_brake : NULL, .clear = in->clear ? motor_clear : NULL,
        .shutdown = motor_shutdown,
    };
    const solar_os_motor_registration_t registration = {
        .name = state->name, .driver = state->driver, .ops = &ops, .ctx = state,
    };
    return provider_finish(state, out, solar_os_native_result(solar_os_motor_register(&registration)));
}

int solar_os_native_motor_provider_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_MOTOR_PROVIDER);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ENCODER
static esp_err_t encoder_read(void *user, int32_t *count)
{
    native_device_provider_t *state = user;
    const solar_os_native_encoder_provider_v1_t *provider = &state->callbacks.encoder;
    const bool held = provider_callback_enter(state);
    return provider_callback_leave(state, held, provider->read(provider->user, count));
}

static esp_err_t encoder_reset(void *user)
{
    native_device_provider_t *state = user;
    return motion_call(state, state->callbacks.encoder.reset, state->callbacks.encoder.user);
}

static esp_err_t encoder_shutdown(void *user)
{
    native_device_provider_t *state = user;
    return motion_shutdown(state, state->callbacks.encoder.shutdown, state->callbacks.encoder.user);
}

int solar_os_native_encoder_provider_register(const solar_os_native_encoder_provider_v1_t *in,
                                             solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->name || !in->driver || !*in->driver || !in->read)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    native_device_provider_t *state;
    int result = provider_create(SOLAR_OS_NATIVE_HANDLE_ENCODER_PROVIDER, in->library_lease,
        in->name, in->driver, out, &state);
    if (result) return result;
    state->callbacks.encoder = *in;
    state->callbacks.encoder.name = state->name;
    state->callbacks.encoder.driver = state->driver;
    const solar_os_encoder_ops_t ops = {
        .read = encoder_read, .reset = in->reset ? encoder_reset : NULL,
        .shutdown = in->shutdown ? encoder_shutdown : NULL,
    };
    const solar_os_encoder_registration_t registration = {
        .name = state->name, .driver = state->driver, .ops = &ops, .mode = in->mode, .ctx = state,
    };
    return provider_finish(state, out, solar_os_native_result(solar_os_encoder_register(&registration)));
}

int solar_os_native_encoder_provider_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_ENCODER_PROVIDER);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
static esp_err_t battery_provider_read(void *user, solar_os_battery_sample_t *sample)
{
    native_device_provider_t *state = user;
    const solar_os_native_battery_provider_v1_t *provider = &state->callbacks.battery;
    const bool held = provider_callback_enter(state);
    return provider_callback_leave(state, held, provider->read(provider->user, sample));
}

static int battery_provider_register(const solar_os_native_battery_provider_v1_t *in,
    solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->owner || !*in->owner || !in->read) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    native_device_provider_t *state;
    int result = provider_create(SOLAR_OS_NATIVE_HANDLE_BATTERY_PROVIDER, in->library_lease,
        NULL, in->owner, out, &state);
    if (result) return result;
    state->callbacks.battery = *in;
    state->callbacks.battery.owner = state->driver;
    const solar_os_battery_provider_t registration = {.read = battery_provider_read, .user = state};
    return provider_finish(state, out,
        solar_os_native_result(solar_os_battery_register_provider(state->driver, &registration)));
}

static int battery_provider_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_BATTERY_PROVIDER);
}

static const solar_os_native_battery_provider_api_v1_t battery_provider_api = {
    .abi_version = SOLAR_OS_NATIVE_BATTERY_PROVIDER_ABI, .struct_size = sizeof(battery_provider_api),
    .register_provider = battery_provider_register, .close = battery_provider_close,
};
#endif
#endif

const void *solar_os_native_battery_provider_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_battery_provider_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}

#if SOLAR_OS_PACKAGE_SERVICE_PWM
static size_t pwm_provider_output_count(void *user)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const size_t count = state->callbacks.pwm.output_count(state->callbacks.pwm.user);
    if (held) solar_os_native_handle_release(state->handle);
    return count;
}

static bool pwm_provider_output(void *user, size_t index, solar_os_pwm_output_info_t *out)
{
    native_device_provider_t *state = user;
    solar_os_native_pwm_info_v1_t info = {0};
    const bool held = provider_callback_enter(state);
    const bool found = state->callbacks.pwm.get_output(state->callbacks.pwm.user, index, &info);
    if (held) solar_os_native_handle_release(state->handle);
    if (!found || info.output > UINT16_MAX || info.frequency_group > UINT16_MAX ||
        info.duty_percent > SOLAR_OS_PWM_DUTY_MAX_PERCENT || info.resolution_bits > UINT8_MAX) {
        return false;
    }
    *out = (solar_os_pwm_output_info_t){
        .output = info.output,
        .frequency_group = info.frequency_group,
        .pin = info.pin,
        .runtime_allowed = info.runtime_allowed != 0,
        .active = info.active != 0,
        .channel = info.channel,
        .freq_hz = info.frequency_hz,
        .duty_percent = info.duty_percent,
        .enabled = info.enabled != 0,
        .resolution_bits = info.resolution_bits,
        .pulse_width_us = info.pulse_width_us,
    };
    return true;
}

static esp_err_t pwm_provider_set(void *user, uint16_t output,
                                  uint32_t frequency, uint32_t value, bool pulse)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.pwm.set(state->callbacks.pwm.user,
                                                output, frequency, value, pulse);
    return provider_callback_leave(state, held, result);
}

static esp_err_t pwm_provider_open(void *user, uint16_t output,
                                   uint32_t frequency, const char *owner)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.pwm.open(state->callbacks.pwm.user,
                                                 output, frequency, owner);
    return provider_callback_leave(state, held, result);
}

static esp_err_t pwm_provider_write(void *user, uint16_t output,
                                    const char *owner, uint32_t pulse)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.pwm.write_pulse(state->callbacks.pwm.user,
                                                        output, owner, pulse);
    return provider_callback_leave(state, held, result);
}

static esp_err_t pwm_provider_disable(void *user, uint16_t output, const char *owner)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.pwm.disable(state->callbacks.pwm.user, output, owner);
    return provider_callback_leave(state, held, result);
}

static esp_err_t pwm_provider_close_output(void *user, uint16_t output, const char *owner)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.pwm.close(state->callbacks.pwm.user, output, owner);
    return provider_callback_leave(state, held, result);
}

static esp_err_t pwm_provider_stop(void *user, uint16_t output)
{
    native_device_provider_t *state = user;
    const bool held = provider_callback_enter(state);
    const int result = state->callbacks.pwm.stop(state->callbacks.pwm.user, output);
    return provider_callback_leave(state, held, result);
}

static int pwm_provider_register(const solar_os_native_pwm_provider_callbacks_v1_t *in,
                                 solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->name || !in->driver || !in->output_count || !in->get_output ||
        !in->set || !in->open || !in->write_pulse || !in->disable || !in->close || !in->stop) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    native_device_provider_t *state;
    int result = provider_create(SOLAR_OS_NATIVE_HANDLE_PWM_PROVIDER, in->library_lease,
                                 in->name, in->driver, out, &state);
    if (result) return result;
    state->callbacks.pwm = *in;
    state->callbacks.pwm.name = state->name;
    state->callbacks.pwm.driver = state->driver;
    static const solar_os_pwm_ops_t ops = {
        .output_count = pwm_provider_output_count,
        .get_output = pwm_provider_output,
        .set = pwm_provider_set,
        .open = pwm_provider_open,
        .write_pulse = pwm_provider_write,
        .disable = pwm_provider_disable,
        .close = pwm_provider_close_output,
        .stop = pwm_provider_stop,
    };
    const solar_os_pwm_registration_t registration = {
        .name = state->name,
        .driver = state->driver,
        .detachable = true,
        .ops = &ops,
        .ctx = state,
    };
    result = solar_os_native_result(solar_os_pwm_register(&registration));
    return provider_finish(state, out, result);
}

static int pwm_provider_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_PWM_PROVIDER);
}

static const solar_os_native_pwm_provider_api_v1_t pwm_provider_api = {
    .abi_version = SOLAR_OS_NATIVE_PWM_PROVIDER_ABI,
    .struct_size = sizeof(pwm_provider_api),
    .register_provider = pwm_provider_register,
    .close = pwm_provider_close,
};
#endif

const void *solar_os_native_pwm_provider_get_service(const char *name, uint32_t abi,
                                                       uint32_t minimum)
{
#include "generated/native_pwm_provider_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
