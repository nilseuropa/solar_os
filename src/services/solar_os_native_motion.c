#include "solar_os_native_motion.h"
#include <math.h>
#include <string.h>
#include "solar_os_config.h"
#include "esp_err.h"
#include "solar_os_native_motion_abi.h"
#if SOLAR_OS_PACKAGE_SERVICE_MOTOR
#include "solar_os_motor.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ENCODER
#include "solar_os_encoder.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SERVO
#include "solar_os_servo.h"
#endif

#if SOLAR_OS_PACKAGE_SERVICE_MOTOR || SOLAR_OS_PACKAGE_SERVICE_ENCODER || SOLAR_OS_PACKAGE_SERVICE_SERVO
static int result(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return SOLAR_OS_NATIVE_OK;
    case ESP_ERR_INVALID_ARG: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    case ESP_ERR_INVALID_STATE: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    case ESP_ERR_NOT_FOUND: return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    case ESP_ERR_NOT_SUPPORTED: return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    case ESP_ERR_NO_MEM: return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    case ESP_ERR_TIMEOUT: return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    default: return SOLAR_OS_NATIVE_ERROR_FAILED;
    }
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_MOTOR
static int motor_claim(const char *name, uint32_t *claim) { return result(solar_os_motor_claim(name, claim)); }
static int motor_drive(uint32_t claim, float value) { return result(solar_os_motor_drive_owned(claim, value)); }
static int motor_stop(uint32_t claim, uint32_t brake) { return result(solar_os_motor_stop_owned(claim, brake != 0)); }
static int motor_release(uint32_t claim, uint32_t brake) { return result(solar_os_motor_release(claim, brake != 0)); }
static int motor_state(const char *name, solar_os_native_motor_state_v1_t *state)
{
    if (state == NULL) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_motor_status_t value;
    esp_err_t err = solar_os_motor_status(name, &value);
    if (err == ESP_OK) *state = (solar_os_native_motor_state_v1_t){
        .blocking_faults = value.blocking_faults, .can_drive = value.info.can_drive, .can_brake = value.info.can_brake};
    return result(err);
}
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ENCODER
static int encoder_claim(const char *name, uint32_t *claim) { return result(solar_os_encoder_claim(name, claim)); }
static int encoder_release(uint32_t claim) { return result(solar_os_encoder_release(claim)); }
static int encoder_sample(uint32_t claim, solar_os_native_encoder_sample_v1_t *sample)
{
    if (sample == NULL) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_encoder_sample_t value;
    esp_err_t err = solar_os_encoder_sample_owned(claim, &value);
    if (err == ESP_OK) *sample = (solar_os_native_encoder_sample_v1_t){
        .count = value.count, .decoding = value.decoding, .timestamp_us = value.timestamp_us};
    return result(err);
}
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SERVO
static int servo_open(const char *name, const char *owner, const solar_os_native_servo_config_v1_t *c)
{
    if (c == NULL || c->output > UINT16_MAX || strnlen(c->provider, sizeof(c->provider)) == sizeof(c->provider))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_servo_config_t value = {.output.output = c->output, .freq_hz = c->frequency_hz,
        .pulse_min_us = c->pulse_min_us, .pulse_max_us = c->pulse_max_us,
        .angle_min_rad = c->angle_min,
        .angle_max_rad = c->angle_max};
    memcpy(value.output.provider, c->provider, sizeof(c->provider));
    return result(solar_os_servo_open_private(name, owner, &value));
}
static int servo_set(const char *name, const char *owner, float value)
{ return result(solar_os_servo_set(name, owner, value)); }
static int servo_disable(const char *name, const char *owner) { return result(solar_os_servo_disable(name, owner)); }
static int servo_close(const char *name, const char *owner) { return result(solar_os_servo_close(name, owner)); }
#endif

static const solar_os_native_motion_api_v1_t api = {
    .abi_version = SOLAR_OS_NATIVE_MOTION_ABI, .struct_size = sizeof(api),
    .capabilities = (SOLAR_OS_PACKAGE_SERVICE_MOTOR ? SOLAR_OS_NATIVE_MOTION_MOTOR : 0) |
        (SOLAR_OS_PACKAGE_SERVICE_ENCODER ? SOLAR_OS_NATIVE_MOTION_ENCODER : 0) |
        (SOLAR_OS_PACKAGE_SERVICE_SERVO ? SOLAR_OS_NATIVE_MOTION_SERVO : 0),
#if SOLAR_OS_PACKAGE_SERVICE_MOTOR
    .motor_claim = motor_claim, .motor_state = motor_state, .motor_drive = motor_drive,
    .motor_stop = motor_stop, .motor_release = motor_release,
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ENCODER
    .encoder_claim = encoder_claim, .encoder_sample = encoder_sample, .encoder_release = encoder_release,
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SERVO
    .servo_open = servo_open, .servo_set = servo_set, .servo_disable = servo_disable, .servo_close = servo_close,
#endif
};
const void *solar_os_native_motion_get_service(const char *name, uint32_t abi, uint32_t size)
{ return name && strcmp(name, SOLAR_OS_NATIVE_MOTION_SERVICE) == 0 && abi == SOLAR_OS_NATIVE_MOTION_ABI && size <= sizeof(api) ? &api : NULL; }
