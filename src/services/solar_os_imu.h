#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_sensors_abi.h"

#define SOLAR_OS_IMU_NAME_MAX 20
#define SOLAR_OS_IMU_DRIVER_MAX 24

/* imu.<device> is a shared byte source. Each read returns one 52-byte
 * imu-v1-le record: uint64 timestamp_us, uint32 valid, then float32 accel[3],
 * gyro[3], orientation[4] (w,x,y,z), all little endian, without padding. Units and
 * validity bits match solar_os_imu_sample_t. No frame ID or sampling worker. */
#define SOLAR_OS_IMU_STREAM_FORMAT "imu-v1-le"
#define SOLAR_OS_IMU_STREAM_SAMPLE_BYTES 52U

typedef solar_os_native_imu_sample_v1_t solar_os_imu_sample_t;

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

typedef solar_os_native_sensor_info_v1_t solar_os_imu_info_t;

esp_err_t solar_os_imu_register(const solar_os_imu_registration_t *registration);
esp_err_t solar_os_imu_unregister(const char *name);
esp_err_t solar_os_imu_unregister_owned(const char *name, const void *context);
size_t solar_os_imu_count(void);
bool solar_os_imu_get(size_t index, solar_os_imu_info_t *info);
esp_err_t solar_os_imu_read_sample(const char *name,
                                   uint32_t timeout_ms,
                                   solar_os_imu_sample_t *sample);
