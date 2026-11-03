#pragma once
#include "solar_os_native_library_abi.h"
#include "solar_os_native_motion_abi.h"

#define SOLAR_OS_ROBOT_LIBRARY "robot"
#define SOLAR_OS_ROBOT_ABI 3U
#define SOLAR_OS_ROBOT_JOINT_MAX 8U
#define SOLAR_OS_ROBOT_JOINT_NAME_MAX 24U

typedef enum { SOLAR_OS_ROBOT_SERVO_POSITION = 1, SOLAR_OS_ROBOT_MOTOR_VELOCITY = 2 } solar_os_robot_joint_kind_t;
typedef enum { SOLAR_OS_ROBOT_READY, SOLAR_OS_ROBOT_ACTIVE, SOLAR_OS_ROBOT_FAULT,
    SOLAR_OS_ROBOT_WAITING } solar_os_robot_joint_state_t;

typedef struct {
    char name[SOLAR_OS_ROBOT_JOINT_NAME_MAX], motor[20], encoder[20];
    uint32_t kind;
    /* PPR = A-channel periods/encoder-shaft revolution. Decoder factor comes
     * from the encoder provider. Ratio = motor rev/output rev, always > 0.
     * Motor joints report zero for either calibration until explicitly set. */
    float ppr, gear_ratio;
    int32_t motor_polarity, encoder_polarity; /* Independently +1 or -1. */
    uint32_t encoder_on_output;
    float position_min, position_max, position_zero;
    float velocity_limit, acceleration_limit, max_drive, velocity_tau;
    /* Continuous friction feedforward magnitudes in joint-positive/negative
     * directions, bounded by max_drive. These are not breakaway thresholds.
     * Command deadband is in rad/s. */
    float friction_bias_forward, friction_bias_reverse, velocity_deadband;
    float kp, ki, kd, derivative_tau;
    uint32_t period_ms, command_timeout_ms, feedback_timeout_ms, max_gap_ms, brake;
    solar_os_native_servo_config_v1_t servo;
} solar_os_robot_joint_config_v3_t;

typedef struct {
    solar_os_robot_joint_config_v3_t config;
    uint32_t state, claimed, command_valid, position_valid, velocity_valid;
    uint32_t referenced, decoding, saturated, steps, faults;
    int32_t raw_count, last_error;
    uint64_t sample_us, command_expires_us;
    /* Read-only: PPR * actual decoding * transmission factor. Zero until
     * calibration and the encoder's decoder mode are known. */
    float encoder_counts_per_rev;
    float target, setpoint, position, velocity, commanded_position, output, feedforward, p, i, d;
} solar_os_robot_joint_info_v3_t;

/* Optional external library. No URDF or graphics dependency. Creation and
 * configuration do not claim outputs. Enable starts a joinable periodic loop;
 * motor output stays stopped until the first fresh command, which arms the
 * command watchdog. Expiry stops output and waits for a new command while
 * retaining ownership. Hardware, feedback and timing faults latch.
 * Disable/close join the worker and
 * stop/release hardware; failed cleanup keeps the object usable for retry.
 * Keep the library lease until close succeeds. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*create)(const char *name, uint32_t kind, uint32_t *joint);
    int (*configure)(uint32_t joint, const char *key, float value);
    int (*bind_motor)(uint32_t joint, const char *motor, const char *encoder);
    int (*bind_servo)(uint32_t joint, const solar_os_native_servo_config_v1_t *config);
    int (*command)(uint32_t joint, float target);
    int (*enable)(uint32_t joint);
    int (*disable)(uint32_t joint);
    int (*status)(uint32_t joint, solar_os_robot_joint_info_v3_t *info);
    int (*close)(uint32_t joint);
} solar_os_robot_api_v3_t;
