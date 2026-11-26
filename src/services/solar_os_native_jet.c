#include "solar_os_native_jet.h"
#include "solar_os_config.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_JET
#include "solar_os.h"
#include "solar_os_jet.h"
#include "solar_os_memory.h"
#include "solar_os_native.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"

#define JET_HANDLE_KIND SOLAR_OS_NATIVE_HANDLE_JET

typedef struct {
    solar_os_jet_t *scene;
    const solar_os_app_t *owner;
    uint32_t callback_library;
    bool ready;
} native_jet_t;

static int jet_destroy(void *opaque)
{
    native_jet_t *state = opaque;
    state->ready = false;
    int result = solar_os_native_result(solar_os_jet_close(&state->scene));
    if (result) return result;
    if (state->callback_library) {
        result = solar_os_native_library_release(state->callback_library);
        if (result) return result;
    }
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int jet_open(const solar_os_jet_options_t *options, uint32_t callback_library,
    solar_os_native_handle_t *out, char *detail, size_t capacity)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!options || (!detail && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (capacity) detail[0] = 0;
    native_jet_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "jet.native");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    state->owner = solar_os_application_descriptor();
    solar_os_native_handle_t handle = 0;
    int result = state->owner ?
        solar_os_native_handle_create_owned(JET_HANDLE_KIND, state, jet_destroy, state->owner, &handle) :
        solar_os_native_handle_create(JET_HANDLE_KIND, state, jet_destroy, &handle);
    if (result) {
        solar_os_memory_free(state);
        return result;
    }
    if (callback_library) result = solar_os_native_library_retain(callback_library, &state->callback_library);
    if (!result) {
        solar_os_context_t *ctx = solar_os_application_context();
        solar_os_gfx_t *gfx = ctx ? solar_os_context_gfx(ctx) : NULL;
        result = solar_os_native_result(solar_os_jet_open(options, gfx, &state->scene, detail, capacity));
    }
    state->ready = result == SOLAR_OS_NATIVE_OK;
    if (!result || state->scene || solar_os_native_handle_close_kind(handle, JET_HANDLE_KIND)) *out = handle;
    return result;
}

static int jet_acquire(solar_os_native_handle_t id, native_jet_t **out)
{
    void *borrowed;
    const int result = solar_os_native_handle_acquire_exclusive(id, JET_HANDLE_KIND, &borrowed);
    if (result) return result;
    native_jet_t *state = borrowed;
    if (!state->ready) {
        solar_os_native_handle_release_exclusive(id);
        return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    *out = state;
    return SOLAR_OS_NATIVE_OK;
}

static int jet_finish(solar_os_native_handle_t id, esp_err_t error)
{
    solar_os_native_handle_release_exclusive(id);
    return solar_os_native_result(error);
}

static int jet_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, JET_HANDLE_KIND); }

static int jet_set_suspended(solar_os_native_handle_t id, bool suspended)
{
    native_jet_t *state;
    const int result = jet_acquire(id, &state);
    return result ? result : jet_finish(id, suspended ? solar_os_jet_suspend(state->scene) :
        solar_os_jet_resume(state->scene));
}

static int jet_uses_foreground(solar_os_native_handle_t id, bool *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = false;
    native_jet_t *state;
    const int result = jet_acquire(id, &state);
    if (result) return result;
    solar_os_context_t *ctx = solar_os_application_context();
    solar_os_gfx_t *gfx = ctx ? solar_os_context_gfx(ctx) : NULL;
    *out = gfx && solar_os_jet_uses_gfx(state->scene, gfx);
    return jet_finish(id, ESP_OK);
}

static int jet_background(solar_os_native_handle_t id, uint32_t rgb888)
{
    native_jet_t *state;
    const int result = jet_acquire(id, &state);
    return result ? result : jet_finish(id, solar_os_jet_background(state->scene, rgb888));
}

static int jet_camera(solar_os_native_handle_t id, const solar_os_jet_pose_t *pose, uint16_t fov)
{
    native_jet_t *state;
    const int result = jet_acquire(id, &state);
    return result ? result : jet_finish(id, solar_os_jet_camera(state->scene, pose, fov));
}

static int jet_light(solar_os_native_handle_t id, uint32_t ambient, uint32_t rgb888,
    uint16_t intensity, int32_t azimuth, int32_t elevation)
{
    native_jet_t *state;
    const int result = jet_acquire(id, &state);
    return result ? result : jet_finish(id, solar_os_jet_light(state->scene, ambient, rgb888,
        intensity, azimuth, elevation));
}

static int jet_cube(solar_os_native_handle_t id, uint16_t width, uint16_t height,
    uint16_t depth, uint32_t rgb888, uint32_t *object)
{
    if (!object) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *object = 0;
    native_jet_t *state;
    const int result = jet_acquire(id, &state);
    return result ? result : jet_finish(id, solar_os_jet_cube(state->scene, width, height, depth, rgb888, object));
}

static int jet_mesh(solar_os_native_handle_t id, const solar_os_jet_vertex_t *vertices, size_t count,
    const solar_os_jet_triangle_t *triangles, size_t triangle_count, uint32_t rgb888, uint32_t *object)
{
    if (!object) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *object = 0;
    native_jet_t *state;
    const int result = jet_acquire(id, &state);
    return result ? result : jet_finish(id, solar_os_jet_mesh(state->scene, vertices, count, triangles,
        triangle_count, rgb888, object));
}

static int jet_transform(solar_os_native_handle_t id, uint32_t object, const solar_os_jet_pose_t *pose)
{
    native_jet_t *state;
    const int result = jet_acquire(id, &state);
    return result ? result : jet_finish(id, solar_os_jet_transform(state->scene, object, pose));
}

static int jet_remove(solar_os_native_handle_t id, uint32_t object)
{
    native_jet_t *state;
    const int result = jet_acquire(id, &state);
    return result ? result : jet_finish(id, solar_os_jet_remove(state->scene, object));
}

static int jet_render(solar_os_native_handle_t id, solar_os_native_cancel_fn cancel, void *user)
{
    native_jet_t *state;
    const int result = jet_acquire(id, &state);
    return result ? result : jet_finish(id, solar_os_jet_render(state->scene, cancel, user));
}

static int jet_status(solar_os_native_handle_t id, solar_os_jet_status_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_jet_status_t){0};
    native_jet_t *state;
    const int result = jet_acquire(id, &state);
    if (result) return result;
    solar_os_jet_status(state->scene, out);
    out->error = solar_os_native_result(out->error);
    return jet_finish(id, ESP_OK);
}

static int jet_execute(solar_os_native_handle_t id, const solar_os_jet_command_t *command,
    solar_os_jet_reply_t *reply)
{
    if (!command || !reply) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *reply = (solar_os_jet_reply_t){0};
    native_jet_t *state;
    const int result = jet_acquire(id, &state);
    if (result) return result;
    if ((command->opcode == SOLAR_OS_JET_SHADER || command->opcode == SOLAR_OS_JET_SHADER_SET) &&
        (command->data || command->bytes) && !state->callback_library &&
        (!state->owner || state->owner != solar_os_application_descriptor())) {
        return jet_finish(id, ESP_ERR_INVALID_STATE);
    }
    return jet_finish(id, solar_os_jet_execute(state->scene, command, reply));
}

static const solar_os_native_jet_api_v1_t jet_api = {
    .abi_version = SOLAR_OS_NATIVE_JET_ABI, .struct_size = sizeof(jet_api),
    .open = jet_open, .close = jet_close, .set_suspended = jet_set_suspended,
    .uses_foreground = jet_uses_foreground, .background = jet_background, .camera = jet_camera,
    .light = jet_light, .cube = jet_cube, .mesh = jet_mesh, .transform = jet_transform,
    .remove = jet_remove, .render = jet_render, .status = jet_status, .execute = jet_execute,
};
#endif

const void *solar_os_native_jet_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_jet_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
