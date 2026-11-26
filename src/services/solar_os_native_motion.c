#include "solar_os_native_motion.h"
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
static int motor_status(const char *name, solar_os_native_motor_status_v1_t *status)
{ return result(solar_os_motor_status(name, status)); }
static int motor_command(const char *name, uint32_t command, float value)
{
    switch (command) {
    case SOLAR_OS_NATIVE_MOTOR_COMMAND_DRIVE: return result(solar_os_motor_drive(name, value));
    case SOLAR_OS_NATIVE_MOTOR_COMMAND_COAST: return result(solar_os_motor_coast(name));
    case SOLAR_OS_NATIVE_MOTOR_COMMAND_BRAKE: return result(solar_os_motor_brake(name));
    case SOLAR_OS_NATIVE_MOTOR_COMMAND_CLEAR_FAULT: return result(solar_os_motor_clear(name));
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
}
static int motor_get_config(const char *name, solar_os_native_motor_config_v1_t *config)
{ return result(solar_os_motor_config_get(name, config)); }
static int motor_configure(const char *name, const char *key, float value)
{ return result(solar_os_motor_configure(name, key, value)); }
static int motor_configure_owned(uint32_t claim, const solar_os_native_motor_config_v1_t *config)
{ return result(solar_os_motor_stream_configure(claim, config)); }
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
{ return result(solar_os_encoder_sample_owned(claim, sample)); }
static int encoder_status(const char *name, solar_os_native_encoder_status_v1_t *status)
{ return result(solar_os_encoder_status(name, status)); }
static int encoder_read(const char *name, int32_t *count)
{ return result(solar_os_encoder_read(name, count)); }
static int encoder_reset(const char *name)
{ return result(solar_os_encoder_reset(name)); }
static int encoder_get_config(const char *name, solar_os_native_encoder_config_v1_t *config)
{ return result(solar_os_encoder_config_get(name, config)); }
static int encoder_configure(const char *name, const char *key, float value)
{ return result(solar_os_encoder_configure(name, key, value)); }
static int encoder_configure_owned(uint32_t claim, const solar_os_native_encoder_config_v1_t *config)
{ return result(solar_os_encoder_stream_configure(claim, config)); }
static int encoder_feedback(uint32_t claim, solar_os_native_encoder_feedback_v1_t *feedback)
{ return result(solar_os_encoder_feedback(claim, feedback)); }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SERVO
static int servo_open_config(const char *name, const char *owner,
    const solar_os_native_servo_config_v1_t *config, bool inverted, bool writable_input)
{
    if (!config || config->output > UINT16_MAX ||
        strnlen(config->provider, sizeof(config->provider)) == sizeof(config->provider))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    solar_os_servo_config_t value = {
        .output.output = config->output, .freq_hz = config->frequency_hz,
        .pulse_min_us = config->pulse_min_us, .pulse_max_us = config->pulse_max_us,
        .angle_min_rad = config->angle_min, .angle_max_rad = config->angle_max, .inverted = inverted,
    };
    memcpy(value.output.provider, config->provider, sizeof(value.output.provider));
    return result(writable_input ? solar_os_servo_open(name, owner, &value) :
        solar_os_servo_open_private(name, owner, &value));
}

static int servo_open(const char *name, const char *owner, const solar_os_native_servo_config_v1_t *config)
{ return servo_open_config(name, owner, config, false, false); }

static int servo_open_ex(const char *name, const char *owner,
    const solar_os_native_servo_config_v2_t *config, bool writable_input)
{
    if (!config) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return servo_open_config(name, owner, &config->calibration, config->inverted, writable_input);
}

static void servo_copy_info(solar_os_native_servo_info_v1_t *out, const solar_os_servo_info_t *info)
{
    _Static_assert(sizeof(out->name) == sizeof(info->name), "native servo name capacity");
    _Static_assert(sizeof(out->owner) == sizeof(info->owner), "native servo owner capacity");
    _Static_assert(sizeof(out->config.calibration.provider) == sizeof(info->config.output.provider),
        "native servo PWM provider capacity");
    const solar_os_servo_config_t *config = &info->config;
    *out = (solar_os_native_servo_info_v1_t){
        .config = {.calibration = {.output = config->output.output, .frequency_hz = config->freq_hz,
            .pulse_min_us = config->pulse_min_us, .pulse_max_us = config->pulse_max_us,
            .angle_min = config->angle_min_rad, .angle_max = config->angle_max_rad},
            .inverted = config->inverted},
        .enabled = info->enabled, .has_command = info->has_command,
        .commanded_pulse_us = info->commanded_pulse_us, .commanded_angle_rad = info->commanded_angle_rad,
        .input_claimed = info->input_claimed, .stream_writable = info->stream_writable, .closing = info->closing,
    };
    memcpy(out->name, info->name, sizeof(out->name));
    memcpy(out->owner, info->owner, sizeof(out->owner));
    memcpy(out->config.calibration.provider, config->output.provider, sizeof(out->config.calibration.provider));
}

static int servo_info(size_t index, solar_os_native_servo_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_servo_info_t info;
    if (!solar_os_servo_get(index, &info)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    servo_copy_info(out, &info);
    return SOLAR_OS_NATIVE_OK;
}

static int servo_status(const char *name, solar_os_native_servo_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_servo_info_t info;
    const esp_err_t error = solar_os_servo_status(name, &info);
    if (error == ESP_OK) servo_copy_info(out, &info);
    return result(error);
}

static int servo_pulse(const char *name, const char *owner, uint32_t pulse_us)
{ return result(solar_os_servo_pulse(name, owner, pulse_us)); }

static int servo_release_owner(const char *owner)
{ return result(solar_os_servo_release_owner(owner)); }

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
    .motor_count = solar_os_motor_count,
    .motor_info = solar_os_motor_get,
    .motor_status = motor_status,
    .motor_command = motor_command,
    .motor_mode_name = solar_os_motor_mode_name,
    .motor_config_valid = solar_os_motor_config_valid,
    .motor_get_config = motor_get_config,
    .motor_configure = motor_configure,
    .motor_configure_owned = motor_configure_owned,
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ENCODER
    .encoder_claim = encoder_claim, .encoder_sample = encoder_sample, .encoder_release = encoder_release,
    .encoder_count = solar_os_encoder_count,
    .encoder_info = solar_os_encoder_get,
    .encoder_status = encoder_status,
    .encoder_read = encoder_read,
    .encoder_reset = encoder_reset,
    .encoder_config_valid = solar_os_encoder_config_valid,
    .encoder_get_config = encoder_get_config,
    .encoder_configure = encoder_configure,
    .encoder_configure_owned = encoder_configure_owned,
    .encoder_feedback = encoder_feedback,
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SERVO
    .servo_open = servo_open, .servo_set = servo_set, .servo_disable = servo_disable, .servo_close = servo_close,
    .servo_open_ex = servo_open_ex, .servo_count = solar_os_servo_count,
    .servo_info = servo_info, .servo_status = servo_status,
    .servo_pulse = servo_pulse, .servo_release_owner = servo_release_owner,
#endif
};
#if SOLAR_OS_PACKAGE_SERVICE_ROBOT
#include "solar_os_robot.h"
#include "solar_os_native_robot_abi.h"
#include "solar_os_native_support.h"

static int native_robot_joint_create(const char *name, uint32_t kind, const char *owner)
{ return solar_os_native_result(solar_os_robot_joint_create(name, kind, owner)); }
static int native_robot_joint_configure(const char *name, const char *key, float value)
{ return solar_os_native_result(solar_os_robot_joint_configure(name, key, value)); }
static int native_robot_joint_bind_motor(const char *name, const char *motor, const char *encoder)
{ return solar_os_native_result(solar_os_robot_joint_bind_motor(name, motor, encoder)); }
static int native_robot_joint_bind_servo(const char *name, const solar_os_native_servo_config_v1_t *config)
{ return solar_os_native_result(solar_os_robot_joint_bind_servo(name, config)); }
static int native_robot_joint_command(const char *name, float target)
{ return solar_os_native_result(solar_os_robot_joint_command(name, target)); }
static int native_robot_joint_status(const char *name, solar_os_robot_joint_info_v3_t *info)
{ return solar_os_native_result(solar_os_robot_joint_status(name, info)); }
static int native_robot_joint_close(const char *name)
{ return solar_os_native_result(solar_os_robot_joint_close(name)); }
static int native_robot_release_owner(const char *owner)
{ return solar_os_native_result(solar_os_robot_release_owner(owner)); }
static int native_robot_joint_set_enabled(const char *name, bool enabled)
{
    return solar_os_native_result(enabled ? solar_os_robot_joint_enable(name) : solar_os_robot_joint_disable(name));
}

static const solar_os_native_robot_joints_api_v1_t robot_joints_api = {
    .abi_version = SOLAR_OS_NATIVE_ROBOT_JOINTS_ABI, .struct_size = sizeof(robot_joints_api),
    .create = native_robot_joint_create,
    .configure = native_robot_joint_configure,
    .bind_motor = native_robot_joint_bind_motor,
    .bind_servo = native_robot_joint_bind_servo,
    .command = native_robot_joint_command,
    .status = native_robot_joint_status,
    .close = native_robot_joint_close,
    .release_owner = native_robot_release_owner,
    .set_enabled = native_robot_joint_set_enabled,
    .count = solar_os_robot_joint_count, .info = solar_os_robot_joint_get_info,
};

static int native_robot_model_load_file(const char *path, char *error, size_t capacity)
{ return solar_os_native_result(solar_os_robot_model_load_file(path, error, capacity)); }
static int native_robot_model_load_xml(const char *xml, size_t bytes, char *error, size_t capacity)
{ return solar_os_native_result(solar_os_robot_model_load_xml(xml, bytes, error, capacity)); }
static int native_robot_model_unload(void)
{ return solar_os_native_result(solar_os_robot_model_unload()); }
static int native_robot_model_get_info(solar_os_robot_model_info_t *info)
{ return solar_os_native_result(solar_os_robot_model_get_info(info)); }
static int native_robot_model_get_link(uint32_t index, char *out, size_t capacity)
{ return solar_os_native_result(solar_os_robot_model_get_link(index, out, capacity)); }
static int native_robot_model_get_joint(uint32_t index, solar_os_robot_model_joint_info_t *out)
{ return solar_os_native_result(solar_os_robot_model_get_joint(index, out)); }
static int native_robot_model_update(const char *const *names, const float *positions, uint32_t count)
{ return solar_os_native_result(solar_os_robot_model_update(names, positions, count)); }
static int native_robot_model_transform(const char *target, const char *source, solar_os_robot_transform_t *out)
{ return solar_os_native_result(solar_os_robot_model_transform(target, source, out)); }
static int native_robot_model_apply(const char *target, const char *source, const float in[3], bool direction, float out[3])
{ return solar_os_native_result(solar_os_robot_model_apply(target, source, in, direction, out)); }
static int native_robot_model_get_extension(char *out, size_t capacity)
{ return solar_os_native_result(solar_os_robot_model_get_extension(out, capacity)); }
static int native_robot_model_get_device(uint32_t index, solar_os_robot_model_device_t *out)
{ return solar_os_native_result(solar_os_robot_model_get_device(index, out)); }
static int native_robot_model_frame(const char *device_or_stream, char *out, size_t capacity)
{ return solar_os_native_result(solar_os_robot_model_frame(device_or_stream, out, capacity)); }
static int native_robot_diff_drive_command(const char *name, float linear, float angular)
{ return solar_os_native_result(solar_os_robot_diff_drive_command(name, linear, angular)); }
static int native_robot_diff_drive_status(const char *name, solar_os_robot_diff_drive_status_t *out)
{ return solar_os_native_result(solar_os_robot_diff_drive_status(name, out)); }
static int native_robot_model_set_running(bool running)
{
    return solar_os_native_result(running ? solar_os_robot_start() : solar_os_robot_stop());
}

static const solar_os_native_robot_model_api_v1_t robot_model_api = {
    .abi_version = SOLAR_OS_NATIVE_ROBOT_MODEL_ABI, .struct_size = sizeof(robot_model_api),
    .load_file = native_robot_model_load_file,
    .load_xml = native_robot_model_load_xml,
    .unload = native_robot_model_unload,
    .info = native_robot_model_get_info,
    .link = native_robot_model_get_link,
    .joint = native_robot_model_get_joint,
    .update = native_robot_model_update,
    .transform = native_robot_model_transform,
    .apply = native_robot_model_apply,
    .extension = native_robot_model_get_extension,
    .device = native_robot_model_get_device,
    .frame = native_robot_model_frame,
    .drive_command = native_robot_diff_drive_command,
    .drive_status = native_robot_diff_drive_status,
    .transform_error = solar_os_robot_model_transform_error, .set_running = native_robot_model_set_running,
};
#endif

const void *solar_os_native_motion_get_service(const char *name, uint32_t abi, uint32_t size)
{
    const uint32_t minimum = size;
#include "generated/native_motion_dispatch.inc"
    return NULL;
}
