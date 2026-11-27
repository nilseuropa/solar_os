#include "solar_os_native_camera.h"
#include "solar_os_native_camera_abi.h"
#include "solar_os_config.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_CAMERA
#include "solar_os_camera.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_device_providers.h"
#include "solar_os_native_support.h"

typedef struct {
    solar_os_camera_owner_t owner;
    const solar_os_camera_frame_t *frame;
} native_camera_t;

static int camera_close_state(void *value)
{
    native_camera_t *camera = value;
    if (camera->frame) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (camera->owner.generation) {
        esp_err_t error = solar_os_camera_stop(&camera->owner);
        if (!error) error = solar_os_camera_release_owner(&camera->owner);
        if (error) return solar_os_native_result(error);
    }
    solar_os_memory_free(camera);
    return SOLAR_OS_NATIVE_OK;
}

static int camera_open(const char *owner, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    native_camera_t *camera = solar_os_memory_calloc(1, sizeof(*camera),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.camera");
    if (!camera) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    int result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_CAMERA, camera, camera_close_state, out);
    if (result) {
        solar_os_memory_free(camera);
        return result;
    }
    result = solar_os_native_result(solar_os_camera_acquire(owner, &camera->owner));
    if (result && !solar_os_native_handle_close_kind(*out, SOLAR_OS_NATIVE_HANDLE_CAMERA)) *out = 0;
    return result;
}

static int camera_close(solar_os_native_handle_t id)
{
    return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_CAMERA);
}

static int camera_set_running(solar_os_native_handle_t id, bool running, const solar_os_camera_config_t *config)
{
    native_camera_t *camera = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_CAMERA);
    if (!camera) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = camera->frame ? SOLAR_OS_NATIVE_ERROR_BUSY : solar_os_native_result(running ?
        solar_os_camera_start(&camera->owner, config) : solar_os_camera_stop(&camera->owner));
    solar_os_native_handle_release(id);
    return result;
}

static int camera_capture(solar_os_native_handle_t id, solar_os_camera_frame_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    native_camera_t *camera = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_CAMERA);
    if (!camera) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = camera->frame ? SOLAR_OS_NATIVE_ERROR_BUSY :
        solar_os_native_result(solar_os_camera_capture(&camera->owner, &camera->frame));
    if (!result) *out = *camera->frame;
    solar_os_native_handle_release(id);
    return result;
}

static int camera_release(solar_os_native_handle_t id)
{
    native_camera_t *camera = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_CAMERA);
    if (!camera) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = solar_os_native_result(solar_os_camera_release_frame(&camera->owner, camera->frame));
    if (!result) camera->frame = NULL;
    solar_os_native_handle_release(id);
    return result;
}

static int camera_invalidate(solar_os_native_handle_t id)
{
    native_camera_t *camera = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_CAMERA);
    if (!camera) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = camera->frame ? SOLAR_OS_NATIVE_ERROR_BUSY :
        solar_os_native_result(solar_os_camera_invalidate(&camera->owner));
    solar_os_native_handle_release(id);
    return result;
}

static int camera_resolutions(solar_os_camera_resolution_t *out, size_t capacity, size_t *count)
{ return solar_os_native_result(solar_os_camera_get_resolutions(out, capacity, count)); }
static int camera_status(solar_os_camera_status_t *out)
{
    const int result = solar_os_native_result(solar_os_camera_get_status(out));
    if (!result) out->last_error = solar_os_native_result(out->last_error);
    return result;
}
static int camera_set_orientation(solar_os_camera_orientation_t orientation)
{ return solar_os_native_result(solar_os_camera_set_orientation(orientation)); }

static const solar_os_native_camera_api_v1_t camera_api = {
    .abi_version = SOLAR_OS_NATIVE_CAMERA_ABI, .struct_size = sizeof(camera_api),
    .default_config = solar_os_camera_default_config, .get_resolutions = camera_resolutions,
    .status = camera_status, .open = camera_open, .set_running = camera_set_running,
    .capture = camera_capture, .release_frame = camera_release, .close = camera_close,
    .invalidate = camera_invalidate, .set_orientation = camera_set_orientation,
    .orientation_name = solar_os_camera_orientation_name,
    .provider_register = solar_os_native_camera_provider_register,
    .provider_close = solar_os_native_camera_provider_close,
};
#endif

const void *solar_os_native_camera_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_camera_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
