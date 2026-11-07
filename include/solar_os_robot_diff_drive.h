#pragma once
#include <stdint.h>

/* Body convention: +X forward, +Z yaw left. Wheel angles/velocities use
 * each calibrated joint's forward-positive convention. All units are SI. */
typedef struct {
    char name[24], left_joint[24], right_joint[24];
    char odom_frame[64], base_frame[64];
    float wheel_radius, wheel_separation, linear_limit, angular_limit;
    uint32_t command_timeout_ms;
} solar_os_robot_diff_drive_config_t;

typedef struct {
    solar_os_robot_diff_drive_config_t config;
    uint32_t state, odometry_valid;
    int32_t last_error;
    float command_linear, command_angular, left_command, right_command;
    double x, y, yaw;
    float linear_velocity, angular_velocity;
    uint64_t command_us[2], sample_us;
} solar_os_robot_diff_drive_status_t;

typedef struct {
    float linear, angular;
    uint64_t linear_us, angular_us;
} solar_os_robot_diff_drive_command_t;
/* Host graph adapters exchange snapshots with the existing joint workers.
 * These callbacks must not block or call back into robot/controllers. The
 * context remains valid until drive_close succeeds. Both callbacks are optional. */
typedef struct {
    void (*command)(void *context,solar_os_robot_diff_drive_command_t *out);
    void (*publish)(void *context,const solar_os_robot_diff_drive_status_t *status);
} solar_os_robot_diff_drive_io_t;
