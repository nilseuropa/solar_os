#pragma once
#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_MOTION_SERVICE "motion"
#define SOLAR_OS_NATIVE_MOTION_ABI 1U
#define SOLAR_OS_NATIVE_MOTION_MOTOR 1U
#define SOLAR_OS_NATIVE_MOTION_ENCODER 2U
#define SOLAR_OS_NATIVE_MOTION_SERVO 4U

typedef struct {
    int32_t count;
    uint32_t decoding; /* 1, 2, or 4 counts per A-channel pulse. */
    uint64_t timestamp_us;
} solar_os_native_encoder_sample_v1_t;

typedef struct {
    uint32_t blocking_faults, can_drive, can_brake;
} solar_os_native_motor_state_v1_t;

typedef struct {
    char provider[20];
    uint32_t output, frequency_hz, pulse_min_us, pulse_max_us;
    float angle_min, angle_max; /* Radians at the servo shaft. */
} solar_os_native_servo_config_v1_t;

/* Generic host device access, without robot/axis policy. Claim tokens pin a
 * device and reject competing writers/reset/detach. Stop/release failures
 * retain ownership for retry. Encoder reads acquire fresh hardware counts. */
typedef struct {
    uint32_t abi_version, struct_size, capabilities;
    int (*motor_claim)(const char *name, uint32_t *claim);
    int (*motor_state)(const char *name, solar_os_native_motor_state_v1_t *state);
    int (*motor_drive)(uint32_t claim, float normalized);
    int (*motor_stop)(uint32_t claim, uint32_t brake);
    int (*motor_release)(uint32_t claim, uint32_t brake);
    int (*encoder_claim)(const char *name, uint32_t *claim);
    int (*encoder_sample)(uint32_t claim, solar_os_native_encoder_sample_v1_t *sample);
    int (*encoder_release)(uint32_t claim);
    int (*servo_open)(const char *name, const char *owner, const solar_os_native_servo_config_v1_t *config);
    int (*servo_set)(const char *name, const char *owner, float radians);
    int (*servo_disable)(const char *name, const char *owner);
    int (*servo_close)(const char *name, const char *owner);
} solar_os_native_motion_api_v1_t;
