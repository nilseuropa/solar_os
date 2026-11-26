#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_SENSORS_SERVICE "sensors"
#define SOLAR_OS_NATIVE_SENSORS_ABI 1U

#define SOLAR_OS_NATIVE_SENSOR_ENVIRONMENT 1U
#define SOLAR_OS_NATIVE_SENSOR_IMU 2U
#define SOLAR_OS_NATIVE_SENSOR_GNSS 3U
#define SOLAR_OS_NATIVE_GNSS_POWER_CONTROL (1U << 0)
#define SOLAR_OS_NATIVE_GNSS_ENABLE_CONTROL (1U << 1)
#define SOLAR_OS_NATIVE_GNSS_POWERED (1U << 0)
#define SOLAR_OS_NATIVE_GNSS_ENABLED (1U << 1)

typedef struct {
    char name[20], driver[24];
    uint32_t capabilities, state; /* Flags are specific to the requested sensor kind. */
} solar_os_native_sensor_info_v1_t;

typedef uint32_t solar_os_sensor_capabilities_t;

typedef enum {
    SOLAR_OS_SENSOR_CAP_TEMPERATURE = 1U << 0,
    SOLAR_OS_SENSOR_CAP_HUMIDITY = 1U << 1,
} solar_os_sensor_capability_t;

typedef struct {
    float temperature_c;
    float humidity_percent;
} solar_os_native_environment_v1_t;

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
    float orientation[4]; /* Quaternion w,x,y,z when ORIENTATION is valid. */
} solar_os_native_imu_sample_v1_t;

typedef struct {
    bool valid;
    bool time_valid;
    bool satellites_valid;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t fix_type;
    uint8_t satellites;
    int32_t latitude_deg_e7;
    int32_t longitude_deg_e7;
    int32_t height_msl_mm;
    uint32_t horizontal_accuracy_mm;
    uint32_t vertical_accuracy_mm;
    int32_t ground_speed_mm_s;
    int32_t heading_deg_e5;
    uint16_t position_dop_e2;
} solar_os_native_gnss_fix_v1_t;

typedef struct {
    bool power_control;
    bool powered;
    bool enable_control;
    bool enabled;
    bool fix_available;
    solar_os_native_gnss_fix_v1_t fix;
} solar_os_native_gnss_status_v1_t;

typedef struct {
    const char *name;
    const char *driver;
    uint32_t kind;
    /* Use zero during a bound driver attach to retain the driver itself. */
    uint32_t library_lease;
    void *user;
    union {
        struct {
            int (*read)(void *user, solar_os_native_environment_v1_t *sample);
            int (*temperature)(void *user, float *degrees_c);
            int (*humidity)(void *user, float *relative_percent);
        } environment;
        struct {
            uint32_t capabilities;
            int (*read)(void *user, uint32_t timeout_ms, solar_os_native_imu_sample_v1_t *sample);
        } imu;
        struct {
            int (*read)(void *user, uint32_t timeout_ms, solar_os_native_gnss_fix_v1_t *fix);
            int (*set_power)(void *user, bool powered);
            int (*set_enabled)(void *user, bool enabled);
            bool powered;
        } gnss;
    } ops;
} solar_os_native_sensor_provider_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    size_t (*count)(uint32_t kind);
    int (*info)(uint32_t kind, size_t index, solar_os_native_sensor_info_v1_t *info);
    /* NULL selects the default provider. Request temperature, humidity or
     * both. Unrequested fields are zero. The default may combine sources. */
    int (*environment)(const char *name, uint32_t fields, solar_os_native_environment_v1_t *sample);
    int (*imu)(const char *name, uint32_t timeout_ms, solar_os_native_imu_sample_v1_t *sample);
    int (*gnss_fix)(const char *name, uint32_t timeout_ms, solar_os_native_gnss_fix_v1_t *fix);
    int (*gnss_status)(const char *name, uint32_t timeout_ms, solar_os_native_gnss_status_v1_t *status);
    /* Select GNSS_POWER_CONTROL or GNSS_ENABLE_CONTROL as the property. */
    int (*gnss_set)(const char *name, uint32_t property, bool enabled);
    /* Copy the provider descriptor and pin its code until close succeeds. */
    int (*provider_register)(const solar_os_native_sensor_provider_v1_t *provider, solar_os_native_handle_t *handle);
    int (*provider_close)(solar_os_native_handle_t handle);
    /* GNSS providers report a physical state change using the same property
     * identifiers as gnss_set. Reporting does not call back into the device. */
    int (*provider_report)(solar_os_native_handle_t handle, uint32_t property, bool enabled);
} solar_os_native_sensors_api_v1_t;
