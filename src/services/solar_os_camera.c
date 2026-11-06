#include "solar_os_camera.h"
#include "solar_os_sync.h"

#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    bool backend_registered;
    bool initialized;
    bool owner_leased;
    bool frame_leased;
    solar_os_camera_backend_t backend;
    char driver[SOLAR_OS_CAMERA_DRIVER_NAME_MAX];
    char owner[SOLAR_OS_CAMERA_OWNER_NAME_MAX];
    uint32_t owner_generation;
    solar_os_camera_sensor_info_t sensor;
    solar_os_camera_config_t config;
    solar_os_camera_orientation_t orientation;
    solar_os_camera_backend_frame_t backend_frame;
    solar_os_camera_frame_t frame;
    uint32_t capture_count;
    esp_err_t last_error;
} camera_service_state_t;

static SemaphoreHandle_t camera_mutex;
static StaticSemaphore_t camera_mutex_storage;
static EXT_RAM_BSS_ATTR camera_service_state_t camera_state;
static uint32_t camera_next_owner_generation;

static esp_err_t ensure_mutex(void)
{
    (void)solar_os_mutex_get(&camera_mutex, &camera_mutex_storage);
    return camera_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool config_valid(const solar_os_camera_config_t *config)
{
    return config != NULL &&
        (config->frame_size == SOLAR_OS_CAMERA_FRAME_SIZE_QVGA ||
         config->frame_size == SOLAR_OS_CAMERA_FRAME_SIZE_VGA) &&
        config->jpeg_quality <= 63U;
}

static bool config_equal(const solar_os_camera_config_t *left,
                         const solar_os_camera_config_t *right)
{
    return left->frame_size == right->frame_size &&
        left->jpeg_quality == right->jpeg_quality;
}

static bool owner_valid(const solar_os_camera_owner_t *token)
{
    return token != NULL && token->generation != 0U &&
        camera_state.owner_leased &&
        token->generation == camera_state.owner_generation;
}

solar_os_camera_config_t solar_os_camera_default_config(void)
{
    return (solar_os_camera_config_t) {
        .frame_size = SOLAR_OS_CAMERA_FRAME_SIZE_QVGA,
        .jpeg_quality = 12U,
    };
}

esp_err_t solar_os_camera_register_backend(
    const solar_os_camera_backend_t *backend)
{
    if (backend == NULL || backend->driver == NULL ||
        backend->driver[0] == '\0' ||
        strnlen(backend->driver, SOLAR_OS_CAMERA_DRIVER_NAME_MAX) >=
            SOLAR_OS_CAMERA_DRIVER_NAME_MAX ||
        backend->ops == NULL || backend->ops->start == NULL ||
        backend->ops->stop == NULL || backend->ops->capture == NULL ||
        backend->ops->release == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_error = ensure_mutex();
    if (mutex_error != ESP_OK) {
        return mutex_error;
    }

    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    if (camera_state.backend_registered) {
        xSemaphoreGive(camera_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    camera_state.backend = *backend;
    camera_state.backend_registered = true;
    strlcpy(camera_state.driver, backend->driver, sizeof(camera_state.driver));
    camera_state.last_error = ESP_OK;
    if (backend->ops->publish != NULL) {
        const esp_err_t error = backend->ops->publish(backend->ctx);
        if (error != ESP_OK) {
            memset(&camera_state, 0, sizeof(camera_state));
            xSemaphoreGive(camera_mutex);
            return error;
        }
    }
    xSemaphoreGive(camera_mutex);
    return ESP_OK;
}

esp_err_t solar_os_camera_unregister_backend(const char *driver)
{
    if (driver == NULL || driver[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_error = ensure_mutex();
    if (mutex_error != ESP_OK) {
        return mutex_error;
    }

    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    if (!camera_state.backend_registered ||
        strcmp(camera_state.driver, driver) != 0) {
        xSemaphoreGive(camera_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (camera_state.initialized || camera_state.owner_leased ||
        camera_state.frame_leased) {
        xSemaphoreGive(camera_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (camera_state.backend.ops->unpublish != NULL) {
        const esp_err_t error = camera_state.backend.ops->unpublish(
            camera_state.backend.ctx);
        if (error != ESP_OK) {
            xSemaphoreGive(camera_mutex);
            return error;
        }
    }
    memset(&camera_state, 0, sizeof(camera_state));
    xSemaphoreGive(camera_mutex);
    return ESP_OK;
}

esp_err_t solar_os_camera_acquire(const char *owner,
                                  solar_os_camera_owner_t *token)
{
    if (owner == NULL || owner[0] == '\0' || token == NULL ||
        strnlen(owner, SOLAR_OS_CAMERA_OWNER_NAME_MAX) >=
            SOLAR_OS_CAMERA_OWNER_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    token->generation = 0U;
    const esp_err_t mutex_error = ensure_mutex();
    if (mutex_error != ESP_OK) {
        return mutex_error;
    }

    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    if (!camera_state.backend_registered) {
        xSemaphoreGive(camera_mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (camera_state.owner_leased) {
        xSemaphoreGive(camera_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    camera_next_owner_generation++;
    if (camera_next_owner_generation == 0U) {
        camera_next_owner_generation++;
    }
    camera_state.owner_leased = true;
    camera_state.owner_generation = camera_next_owner_generation;
    strlcpy(camera_state.owner, owner, sizeof(camera_state.owner));
    token->generation = camera_state.owner_generation;
    xSemaphoreGive(camera_mutex);
    return ESP_OK;
}

esp_err_t solar_os_camera_release_owner(solar_os_camera_owner_t *token)
{
    if (token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_error = ensure_mutex();
    if (mutex_error != ESP_OK) {
        return mutex_error;
    }

    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    if (!owner_valid(token)) {
        xSemaphoreGive(camera_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (camera_state.frame_leased) {
        xSemaphoreGive(camera_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    camera_state.owner_leased = false;
    camera_state.owner_generation = 0U;
    camera_state.owner[0] = '\0';
    token->generation = 0U;
    xSemaphoreGive(camera_mutex);
    return ESP_OK;
}

esp_err_t solar_os_camera_start(const solar_os_camera_owner_t *token,
                                const solar_os_camera_config_t *config)
{
    if (!config_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_error = ensure_mutex();
    if (mutex_error != ESP_OK) {
        return mutex_error;
    }

    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    if (!owner_valid(token)) {
        xSemaphoreGive(camera_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (camera_state.frame_leased) {
        xSemaphoreGive(camera_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (camera_state.initialized && config_equal(&camera_state.config, config)) {
        xSemaphoreGive(camera_mutex);
        return ESP_OK;
    }

    if (camera_state.initialized) {
        const esp_err_t stop_error =
            camera_state.backend.ops->stop(camera_state.backend.ctx);
        if (stop_error != ESP_OK) {
            camera_state.last_error = stop_error;
            xSemaphoreGive(camera_mutex);
            return stop_error;
        }
        camera_state.initialized = false;
        memset(&camera_state.sensor, 0, sizeof(camera_state.sensor));
    }

    solar_os_camera_sensor_info_t sensor = {0};
    const esp_err_t start_error = camera_state.backend.ops->start(
        camera_state.backend.ctx, config, camera_state.orientation, &sensor);
    camera_state.last_error = start_error;
    if (start_error == ESP_OK) {
        camera_state.config = *config;
        camera_state.sensor = sensor;
        camera_state.initialized = true;
    }
    xSemaphoreGive(camera_mutex);
    return start_error;
}

esp_err_t solar_os_camera_stop(const solar_os_camera_owner_t *token)
{
    const esp_err_t mutex_error = ensure_mutex();
    if (mutex_error != ESP_OK) {
        return mutex_error;
    }
    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    if (!owner_valid(token)) {
        xSemaphoreGive(camera_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (camera_state.frame_leased) {
        xSemaphoreGive(camera_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!camera_state.initialized) {
        xSemaphoreGive(camera_mutex);
        return ESP_OK;
    }
    const esp_err_t error =
        camera_state.backend.ops->stop(camera_state.backend.ctx);
    camera_state.last_error = error;
    if (error == ESP_OK) {
        camera_state.initialized = false;
        memset(&camera_state.sensor, 0, sizeof(camera_state.sensor));
    }
    xSemaphoreGive(camera_mutex);
    return error;
}

esp_err_t solar_os_camera_capture(const solar_os_camera_owner_t *token,
                                  const solar_os_camera_frame_t **frame)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *frame = NULL;
    const esp_err_t mutex_error = ensure_mutex();
    if (mutex_error != ESP_OK) {
        return mutex_error;
    }
    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    if (!owner_valid(token) || !camera_state.initialized ||
        camera_state.frame_leased) {
        xSemaphoreGive(camera_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&camera_state.backend_frame, 0, sizeof(camera_state.backend_frame));
    const esp_err_t error = camera_state.backend.ops->capture(
        camera_state.backend.ctx, &camera_state.backend_frame);
    camera_state.last_error = error;
    if (error != ESP_OK) {
        xSemaphoreGive(camera_mutex);
        return error;
    }
    if (camera_state.backend_frame.data == NULL ||
        camera_state.backend_frame.length == 0U ||
        camera_state.backend_frame.release_token == NULL) {
        if (camera_state.backend_frame.release_token != NULL) {
            camera_state.backend.ops->release(
                camera_state.backend.ctx,
                camera_state.backend_frame.release_token);
        }
        memset(&camera_state.backend_frame, 0, sizeof(camera_state.backend_frame));
        camera_state.last_error = ESP_ERR_INVALID_RESPONSE;
        xSemaphoreGive(camera_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }

    camera_state.frame = (solar_os_camera_frame_t) {
        .data = camera_state.backend_frame.data,
        .length = camera_state.backend_frame.length,
        .width = camera_state.backend_frame.width,
        .height = camera_state.backend_frame.height,
        .timestamp_us = camera_state.backend_frame.timestamp_us,
    };
    camera_state.frame_leased = true;
    camera_state.capture_count++;
    *frame = &camera_state.frame;
    xSemaphoreGive(camera_mutex);
    return ESP_OK;
}

esp_err_t solar_os_camera_release_frame(
    const solar_os_camera_owner_t *token,
    const solar_os_camera_frame_t *frame)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_error = ensure_mutex();
    if (mutex_error != ESP_OK) {
        return mutex_error;
    }
    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    if (!owner_valid(token)) {
        xSemaphoreGive(camera_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!camera_state.frame_leased || frame != &camera_state.frame) {
        xSemaphoreGive(camera_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    camera_state.backend.ops->release(camera_state.backend.ctx,
                                      camera_state.backend_frame.release_token);
    memset(&camera_state.backend_frame, 0, sizeof(camera_state.backend_frame));
    memset(&camera_state.frame, 0, sizeof(camera_state.frame));
    camera_state.frame_leased = false;
    xSemaphoreGive(camera_mutex);
    return ESP_OK;
}

esp_err_t solar_os_camera_get_status(solar_os_camera_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_error = ensure_mutex();
    if (mutex_error != ESP_OK) {
        return mutex_error;
    }
    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    *status = (solar_os_camera_status_t) {
        .backend_registered = camera_state.backend_registered,
        .initialized = camera_state.initialized,
        .owner_leased = camera_state.owner_leased,
        .frame_leased = camera_state.frame_leased,
        .sensor = camera_state.sensor,
        .config = camera_state.config,
        .orientation = camera_state.orientation,
        .capture_count = camera_state.capture_count,
        .last_error = camera_state.last_error,
    };
    strlcpy(status->driver, camera_state.driver, sizeof(status->driver));
    strlcpy(status->owner, camera_state.owner, sizeof(status->owner));
    xSemaphoreGive(camera_mutex);
    return ESP_OK;
}

esp_err_t solar_os_camera_set_orientation(solar_os_camera_orientation_t orientation)
{
    if (orientation < SOLAR_OS_CAMERA_ORIENTATION_NORMAL ||
        orientation > SOLAR_OS_CAMERA_ORIENTATION_180) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t mutex_error = ensure_mutex();
    if (mutex_error != ESP_OK) return mutex_error;
    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    esp_err_t error = ESP_OK;
    if (!camera_state.backend_registered ||
        camera_state.backend.ops->set_orientation == NULL) {
        error = ESP_ERR_NOT_SUPPORTED;
    } else if (camera_state.initialized) {
        /* The lock excludes capture/start/stop. A leased JPEG is immutable and
         * can still be sent while changing the orientation of future frames. */
        error = camera_state.backend.ops->set_orientation(
            camera_state.backend.ctx, orientation, &camera_state.orientation);
    } else {
        camera_state.orientation = orientation;
    }
    camera_state.last_error = error;
    xSemaphoreGive(camera_mutex);
    return error;
}

const char *solar_os_camera_orientation_name(solar_os_camera_orientation_t orientation)
{
    switch (orientation) {
    case SOLAR_OS_CAMERA_ORIENTATION_NORMAL: return "normal";
    case SOLAR_OS_CAMERA_ORIENTATION_MIRROR: return "mirror";
    case SOLAR_OS_CAMERA_ORIENTATION_FLIP: return "flip";
    case SOLAR_OS_CAMERA_ORIENTATION_180: return "180";
    default: return "unknown";
    }
}

const char *solar_os_camera_frame_size_name(
    solar_os_camera_frame_size_t frame_size)
{
    switch (frame_size) {
    case SOLAR_OS_CAMERA_FRAME_SIZE_QVGA: return "qvga";
    case SOLAR_OS_CAMERA_FRAME_SIZE_VGA: return "vga";
    default: return "unknown";
    }
}
