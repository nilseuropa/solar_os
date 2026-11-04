#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_CAMERA_SENSOR_NAME_MAX 16U
#define SOLAR_OS_CAMERA_DRIVER_NAME_MAX 24U
#define SOLAR_OS_CAMERA_OWNER_NAME_MAX 24U
#define SOLAR_OS_CAMERA_CAPTURE_TIMEOUT_MS 4000U

typedef enum {
    SOLAR_OS_CAMERA_FRAME_SIZE_QVGA,
    SOLAR_OS_CAMERA_FRAME_SIZE_VGA,
} solar_os_camera_frame_size_t;

typedef struct {
    solar_os_camera_frame_size_t frame_size;
    uint8_t jpeg_quality;
} solar_os_camera_config_t;

typedef struct {
    uint16_t product_id;
    char name[SOLAR_OS_CAMERA_SENSOR_NAME_MAX];
    uint8_t frame_buffer_count;
} solar_os_camera_sensor_info_t;

typedef struct {
    const uint8_t *data;
    size_t length;
    uint16_t width;
    uint16_t height;
    uint64_t timestamp_us;
    void *release_token;
} solar_os_camera_backend_frame_t;

typedef struct {
    esp_err_t (*start)(void *ctx,
                       const solar_os_camera_config_t *config,
                       solar_os_camera_sensor_info_t *sensor);
    esp_err_t (*stop)(void *ctx);
    esp_err_t (*capture)(void *ctx, solar_os_camera_backend_frame_t *frame);
    void (*release)(void *ctx, void *release_token);
    /* Registry lifecycle hooks run under the camera lock and must not call
     * camera APIs. Failure leaves registration unchanged. */
    esp_err_t (*publish)(void *ctx);
    esp_err_t (*unpublish)(void *ctx);
} solar_os_camera_backend_ops_t;

typedef struct {
    const char *driver;
    const solar_os_camera_backend_ops_t *ops;
    void *ctx;
} solar_os_camera_backend_t;

typedef struct {
    const uint8_t *data;
    size_t length;
    uint16_t width;
    uint16_t height;
    uint64_t timestamp_us;
} solar_os_camera_frame_t;

typedef struct {
    uint32_t generation;
} solar_os_camera_owner_t;

typedef struct {
    bool backend_registered;
    bool initialized;
    bool owner_leased;
    bool frame_leased;
    char driver[SOLAR_OS_CAMERA_DRIVER_NAME_MAX];
    char owner[SOLAR_OS_CAMERA_OWNER_NAME_MAX];
    solar_os_camera_sensor_info_t sensor;
    solar_os_camera_config_t config;
    uint32_t capture_count;
    esp_err_t last_error;
} solar_os_camera_status_t;

solar_os_camera_config_t solar_os_camera_default_config(void);
esp_err_t solar_os_camera_register_backend(
    const solar_os_camera_backend_t *backend);
esp_err_t solar_os_camera_unregister_backend(const char *driver);
esp_err_t solar_os_camera_acquire(const char *owner,
                                  solar_os_camera_owner_t *token);
esp_err_t solar_os_camera_release_owner(solar_os_camera_owner_t *token);
esp_err_t solar_os_camera_start(const solar_os_camera_owner_t *token,
                                const solar_os_camera_config_t *config);
esp_err_t solar_os_camera_stop(const solar_os_camera_owner_t *token);
esp_err_t solar_os_camera_capture(const solar_os_camera_owner_t *token,
                                  const solar_os_camera_frame_t **frame);
esp_err_t solar_os_camera_release_frame(
    const solar_os_camera_owner_t *token,
    const solar_os_camera_frame_t *frame);
esp_err_t solar_os_camera_get_status(solar_os_camera_status_t *status);
const char *solar_os_camera_frame_size_name(
    solar_os_camera_frame_size_t frame_size);
