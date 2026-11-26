#pragma once

#include "solar_os_robot_abi.h"

/* These tables operate on the same named joints and resident model as the
 * shell. They acquire the optional Robot library on demand. Built-in and
 * portable callers share validation, controls and cleanup. */

#define SOLAR_OS_NATIVE_ROBOT_JOINTS_SERVICE "robot.joints"
#define SOLAR_OS_NATIVE_ROBOT_JOINTS_ABI 1U

typedef struct {
    uint32_t abi_version, struct_size;
    int (*create)(const char *name, uint32_t kind, const char *owner);
    int (*configure)(const char *name, const char *key, float value);
    int (*bind_motor)(const char *name, const char *motor, const char *encoder);
    int (*bind_servo)(const char *name, const solar_os_native_servo_config_v1_t *config);
    int (*command)(const char *name, float target);
    int (*status)(const char *name, solar_os_robot_joint_info_v3_t *info);
    int (*close)(const char *name);
    int (*release_owner)(const char *owner);
    int (*set_enabled)(const char *name, bool enabled);
    size_t (*count)(void);
    bool (*info)(size_t index, solar_os_robot_joint_info_v3_t *info);
} solar_os_native_robot_joints_api_v1_t;

#define SOLAR_OS_NATIVE_ROBOT_MODEL_SERVICE "robot.model"
#define SOLAR_OS_NATIVE_ROBOT_MODEL_ABI 1U

typedef struct {
    uint32_t abi_version, struct_size;
    int (*load_file)(const char *path, char *error, size_t capacity);
    int (*load_xml)(const char *xml, size_t bytes, char *error, size_t capacity);
    int (*unload)(void);
    int (*info)(solar_os_robot_model_info_t *info);
    int (*link)(uint32_t index, char *out, size_t capacity);
    int (*joint)(uint32_t index, solar_os_robot_model_joint_info_t *out);
    int (*update)(const char *const *names, const float *positions, uint32_t count);
    int (*transform)(const char *target, const char *source, solar_os_robot_transform_t *out);
    int (*apply)(const char *target, const char *source, const float in[3], bool direction, float out[3]);
    int (*extension)(char *out, size_t capacity);
    int (*device)(uint32_t index, solar_os_robot_model_device_t *out);
    int (*frame)(const char *device_or_stream, char *out, size_t capacity);
    int (*drive_command)(const char *name, float linear, float angular);
    int (*drive_status)(const char *name, solar_os_robot_diff_drive_status_t *out);
    void (*transform_error)(const char *target, const char *source, char *error, size_t capacity);
    int (*set_running)(bool running);
} solar_os_native_robot_model_api_v1_t;
