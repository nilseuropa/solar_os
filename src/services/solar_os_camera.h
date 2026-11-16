#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#define SOLAR_OS_CAMERA_SENSOR_NAME_MAX 16U
#define SOLAR_OS_CAMERA_DRIVER_NAME_MAX 24U
#define SOLAR_OS_CAMERA_OWNER_NAME_MAX 24U
#define SOLAR_OS_CAMERA_CAPTURE_TIMEOUT_MS 4000U

typedef struct {
    uint16_t width;
    uint16_t height;
} solar_os_camera_resolution_t;

typedef enum {
    SOLAR_OS_CAMERA_ORIENTATION_NORMAL = 0,
    SOLAR_OS_CAMERA_ORIENTATION_MIRROR = 1,
    SOLAR_OS_CAMERA_ORIENTATION_FLIP = 2,
    SOLAR_OS_CAMERA_ORIENTATION_180 = 3,
} solar_os_camera_orientation_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t jpeg_quality;
} solar_os_camera_config_t;

typedef struct {
    uint16_t product_id;
    char name[SOLAR_OS_CAMERA_SENSOR_NAME_MAX];
    uint8_t frame_buffer_count;
    bool jpeg_quality_fixed;
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
                       solar_os_camera_orientation_t orientation,
                       solar_os_camera_sensor_info_t *sensor);
    esp_err_t (*stop)(void *ctx);
    esp_err_t (*capture)(void *ctx, solar_os_camera_backend_frame_t *frame);
    void (*release)(void *ctx, void *release_token);
    /* Optional live sensor control. Always report the native sensor's cached
     * orientation, even on failure. Existing JPEG leases stay valid. */
    esp_err_t (*set_orientation)(void *ctx,
                                 solar_os_camera_orientation_t orientation,
                                 solar_os_camera_orientation_t *applied);
    /* Registry lifecycle hooks run under the camera lock and must not call
     * camera APIs. Failure leaves registration unchanged. */
    esp_err_t (*publish)(void *ctx);
    esp_err_t (*unpublish)(void *ctx);
    /* Sensor modes may require start() to identify the attached hardware.
     * Always return the full count; copy at most capacity entries. */
    esp_err_t (*get_resolutions)(void *ctx, solar_os_camera_resolution_t *out,
                                 size_t capacity, size_t *count);
    /* Optional protocol extension. Runs under the camera lock with a valid
     * owner and no outstanding frame. Must not call camera service APIs. */
    esp_err_t (*control)(void *ctx, const char *protocol, const void *request);
    /* Optional local cleanup after transport loss; no hardware I/O. The next
     * start must recover/reinitialize the device before returning a frame. */
    void (*invalidate)(void *ctx);
} solar_os_camera_backend_ops_t;

typedef struct {
    const char *driver;
    const solar_os_camera_backend_ops_t *ops;
    void *ctx;
    /* Optional driver default, copied at registration. */
    const solar_os_camera_config_t *default_config;
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
    solar_os_camera_orientation_t orientation;
    uint32_t capture_count;
    esp_err_t last_error;
} solar_os_camera_status_t;

solar_os_camera_config_t solar_os_camera_default_config(void);
/* Also usable by generic video consumers without linking the camera service. */
static inline bool solar_os_camera_parse_resolution(const char *text, uint16_t *width, uint16_t *height)
{
    if (text == NULL || width == NULL || height == NULL) return false;
    if (strcmp(text, "qvga") == 0) { *width = 320U; *height = 240U; return true; }
    if (strcmp(text, "vga") == 0) { *width = 640U; *height = 480U; return true; }
    uint32_t dimensions[2] = {0};
    const char *p = text;
    for (unsigned i = 0; i < 2; i++) {
        if (*p < '0' || *p > '9') return false;
        do {
            dimensions[i] = dimensions[i] * 10U + (unsigned)(*p++ - '0');
            if (dimensions[i] > UINT16_MAX) return false;
        } while (*p >= '0' && *p <= '9');
        if (!dimensions[i] || (i == 0 ? *p++ != 'x' : *p != '\0')) return false;
    }
    *width = dimensions[0]; *height = dimensions[1];
    return true;
}
esp_err_t solar_os_camera_get_resolutions(solar_os_camera_resolution_t *out,
                                          size_t capacity, size_t *count);
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
esp_err_t solar_os_camera_control(const solar_os_camera_owner_t *token,
    const char *protocol, const void *request);
esp_err_t solar_os_camera_invalidate(const solar_os_camera_owner_t *token);
/* Runtime control, independent of the capture owner. Preserved across stop/start. */
esp_err_t solar_os_camera_set_orientation(solar_os_camera_orientation_t orientation);
const char *solar_os_camera_orientation_name(solar_os_camera_orientation_t orientation);
