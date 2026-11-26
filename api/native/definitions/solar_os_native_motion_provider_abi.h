#pragma once

#include "solar_os_native_motion_abi.h"

#define SOLAR_OS_NATIVE_MOTOR_PROVIDER_SERVICE "motor.provider"
#define SOLAR_OS_NATIVE_MOTOR_PROVIDER_ABI 1U
#define SOLAR_OS_NATIVE_ENCODER_PROVIDER_SERVICE "encoder.provider"
#define SOLAR_OS_NATIVE_ENCODER_PROVIDER_ABI 1U

/* Drivers supply initialized device state. Registration copies names and
 * callbacks. A library lease pins callback code. Zero selects the currently
 * attaching native driver. Keep user state alive until close succeeds.
 * The registry serializes callbacks for one device. Close is retryable
 * while an operation or claim holds the device or shutdown returns an error.
 * Run callbacks outside the registry lock, without re-entering the same device.
 * Motor status and coast and encoder read are required. Drive accepts finite
 * values from -1 to 1. Clear leaves the motor coasting. Shutdown is optional
 * and replaces coast during motor close. Encoder read samples a fresh count. */
typedef struct {
    const char *name;
    const char *driver;
    solar_os_native_handle_t library_lease;
    void *user;
    int (*status)(void *user, solar_os_native_motor_status_v1_t *status);
    int (*drive)(void *user, float demand);
    int (*coast)(void *user);
    int (*brake)(void *user);
    int (*clear)(void *user);
    int (*shutdown)(void *user);
} solar_os_native_motor_provider_v1_t;

typedef struct {
    const char *name;
    const char *driver;
    solar_os_native_handle_t library_lease;
    void *user;
    solar_os_native_encoder_mode_v1_t mode;
    int (*read)(void *user, int32_t *count);
    int (*reset)(void *user);
    int (*shutdown)(void *user);
} solar_os_native_encoder_provider_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    int (*register_provider)(const solar_os_native_motor_provider_v1_t *provider,
                             solar_os_native_handle_t *registration);
    int (*close)(solar_os_native_handle_t registration);
} solar_os_native_motor_provider_api_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    int (*register_provider)(const solar_os_native_encoder_provider_v1_t *provider,
                             solar_os_native_handle_t *registration);
    int (*close)(solar_os_native_handle_t registration);
} solar_os_native_encoder_provider_api_v1_t;
