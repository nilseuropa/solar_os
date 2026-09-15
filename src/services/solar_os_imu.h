#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_IMU_NAME_MAX 20
#define SOLAR_OS_IMU_DRIVER_MAX 24

typedef uint32_t solar_os_imu_capabilities_t;

typedef enum {
    SOLAR_OS_IMU_CAP_ACCELERATION = 1U << 0,
    SOLAR_OS_IMU_CAP_ANGULAR_VELOCITY = 1U << 1,
    SOLAR_OS_IMU_CAP_ORIENTATION = 1U << 2,
} solar_os_imu_capability_t;

typedef struct {
    uint64_t timestamp_us;
    solar_os_imu_capabilities_t valid;
    float acceleration_m_s2[3];
    float angular_velocity_rad_s[3];
    float orientation[4];
} solar_os_imu_sample_t;

typedef struct {
    esp_err_t (*read_sample)(void *ctx,
                             uint32_t timeout_ms,
                             solar_os_imu_sample_t *sample);
} solar_os_imu_ops_t;

typedef struct {
    const char *name;
    const char *driver;
    solar_os_imu_capabilities_t capabilities;
    const solar_os_imu_ops_t *ops;
    void *ctx;
} solar_os_imu_registration_t;

typedef struct {
    char name[SOLAR_OS_IMU_NAME_MAX];
    char driver[SOLAR_OS_IMU_DRIVER_MAX];
    solar_os_imu_capabilities_t capabilities;
} solar_os_imu_info_t;

esp_err_t solar_os_imu_register(const solar_os_imu_registration_t *registration);
esp_err_t solar_os_imu_unregister(const char *name);
size_t solar_os_imu_count(void);
bool solar_os_imu_get(size_t index, solar_os_imu_info_t *info);
esp_err_t solar_os_imu_read_sample(const char *name,
                                   uint32_t timeout_ms,
                                   solar_os_imu_sample_t *sample);
