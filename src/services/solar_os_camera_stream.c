#include "solar_os_camera_stream.h"

#include <string.h>

#include "solar_os_camera.h"
#include "solar_os_stream.h"

static solar_os_camera_owner_t stream_owner(const solar_os_stream_handle_t *handle)
{
    return (solar_os_camera_owner_t){.generation = handle->private_data[0]};
}

static esp_err_t camera_open(void *user, const char *owner,
                              const solar_os_stream_open_options_t *options,
                              solar_os_stream_handle_t *handle)
{
    (void)user;
    solar_os_camera_config_t config = solar_os_camera_default_config();
    const solar_os_stream_video_format_t *requested = &options->requested_video;
    if (requested->codec != SOLAR_OS_STREAM_VIDEO_JPEG) return ESP_ERR_NOT_SUPPORTED;
    if (requested->width != 0U || requested->height != 0U) {
        if (!requested->width || !requested->height) return ESP_ERR_INVALID_ARG;
        config.width = requested->width;
        config.height = requested->height;
    }
    if (requested->jpeg_quality != 0U) config.jpeg_quality = requested->jpeg_quality;
    if (config.jpeg_quality > 63U) return ESP_ERR_INVALID_ARG;
    solar_os_camera_owner_t token = {0};
    esp_err_t error = solar_os_camera_acquire(owner, &token);
    if (error != ESP_OK) return error;
    error = solar_os_camera_start(&token, &config);
    if (error != ESP_OK) {
        (void)solar_os_camera_release_owner(&token);
        return error;
    }
    handle->private_data[0] = token.generation;
    handle->video = (solar_os_stream_video_format_t){
        .codec = SOLAR_OS_STREAM_VIDEO_JPEG,
        .width = config.width,
        .height = config.height,
        .jpeg_quality = config.jpeg_quality,
    };
    return ESP_OK;
}

static esp_err_t camera_close(void *user, solar_os_stream_handle_t *handle)
{
    (void)user;
    solar_os_camera_owner_t token = stream_owner(handle);
    esp_err_t error = solar_os_camera_stop(&token);
    if (error == ESP_OK) error = solar_os_camera_release_owner(&token);
    return error;
}

static esp_err_t camera_acquire_frame(void *user, solar_os_stream_handle_t *handle,
                                      solar_os_stream_video_frame_t *frame)
{
    (void)user;
    const solar_os_camera_owner_t token = stream_owner(handle);
    const solar_os_camera_frame_t *captured = NULL;
    const esp_err_t error = solar_os_camera_capture(&token, &captured);
    if (error != ESP_OK) return error;
    handle->private_data[1] = (uintptr_t)captured;
    *frame = (solar_os_stream_video_frame_t){
        .data = captured->data, .length = captured->length,
        .width = captured->width, .height = captured->height,
        .timestamp_us = captured->timestamp_us,
    };
    return ESP_OK;
}

static esp_err_t camera_release_frame(void *user, solar_os_stream_handle_t *handle,
                                      solar_os_stream_video_frame_t *frame)
{
    (void)user;
    (void)frame;
    const solar_os_camera_owner_t token = stream_owner(handle);
    const esp_err_t error = solar_os_camera_release_frame(
        &token, (const solar_os_camera_frame_t *)handle->private_data[1]);
    if (error == ESP_OK) handle->private_data[1] = 0U;
    return error;
}

esp_err_t solar_os_camera_stream_register(const char *name)
{
    const solar_os_camera_config_t config = {.width = 320U, .height = 240U, .jpeg_quality = 12U};
    return solar_os_camera_stream_register_config(name, &config);
}

esp_err_t solar_os_camera_stream_register_owned(const char *name, const solar_os_camera_config_t *config, void *owner)
{
    if (config == NULL || name == NULL || name[0] == '\0' ||
        strnlen(name, SOLAR_OS_STREAM_DEVICE_MAX) >= SOLAR_OS_STREAM_DEVICE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_stream_driver_t driver = {
        .info = {
            .provider = "camera", .type = SOLAR_OS_STREAM_TYPE_VIDEO,
            .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
            .sharing = SOLAR_OS_STREAM_SHARING_EXCLUSIVE,
            .unit = "frame", .format = "jpeg", .summary = "JPEG camera",
            .video = {.codec = SOLAR_OS_STREAM_VIDEO_JPEG,
                      .width = config->width, .height = config->height,
                      .jpeg_quality = config->jpeg_quality},
        },
        .open = camera_open, .close_checked = camera_close, .user = owner,
        .acquire_frame = camera_acquire_frame, .release_frame = camera_release_frame,
    };
    strlcpy(driver.info.id, name, sizeof(driver.info.id));
    strlcpy(driver.info.device, name, sizeof(driver.info.device));
    return solar_os_stream_register(&driver);
}

esp_err_t solar_os_camera_stream_register_config(const char *name, const solar_os_camera_config_t *config)
{
    return solar_os_camera_stream_register_owned(name, config, NULL);
}

esp_err_t solar_os_camera_stream_unregister_owned(const char *name, const void *owner)
{
    return solar_os_stream_unregister_many_owned(&name, &owner, 1);
}

esp_err_t solar_os_camera_stream_unregister(const char *name)
{
    return solar_os_stream_unregister(name);
}
