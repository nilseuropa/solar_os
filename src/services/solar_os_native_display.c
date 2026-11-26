#include "solar_os_native_display.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_display_abi.h"
#include "solar_os_native_display_layer_abi.h"
#include "solar_os_native_display_layout_abi.h"
#include "solar_os_native_display_lease_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_display.h"
#include "solar_os_display_layout.h"
#include "solar_os_frame_presenter.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_sessions.h"

static void display_info_copy(solar_os_native_display_info_v1_t *out,
                              const solar_os_display_target_t *target)
{
    strlcpy(out->name, target->name, sizeof(out->name));
    strlcpy(out->source, target->source, sizeof(out->source));
    strlcpy(out->driver, target->driver, sizeof(out->driver));
    strlcpy(out->controller, target->controller, sizeof(out->controller));
    strlcpy(out->role, target->role, sizeof(out->role));
    strlcpy(out->owner, target->owner, sizeof(out->owner));
    out->panel = target->panel;
    out->canvas = target->canvas;
    out->width = target->width;
    out->height = target->height;
    out->native_width = target->native_width;
    out->native_height = target->native_height;
    out->preferred_stream_fps = target->preferred_stream_fps;
    out->presentation_stack_bytes = target->presentation_stack_bytes;
    out->max_stream_pixels_per_second = target->max_stream_pixels_per_second;
    out->surface_formats = target->surface_formats;
    out->frame_formats = target->frame_formats;
    out->flags =
        (target->ready ? SOLAR_OS_NATIVE_DISPLAY_READY : 0U) |
        (target->round ? SOLAR_OS_NATIVE_DISPLAY_ROUND : 0U) |
        (target->epaper ? SOLAR_OS_NATIVE_DISPLAY_EPAPER : 0U) |
        (target->black_is_one ? SOLAR_OS_NATIVE_DISPLAY_BLACK_IS_ONE : 0U) |
        (target->brightness_supported ? SOLAR_OS_NATIVE_DISPLAY_BRIGHTNESS : 0U) |
        (target->prefer_mono ? SOLAR_OS_NATIVE_DISPLAY_PREFER_MONO : 0U) |
        (solar_os_display_target_is_board_primary(target->name) ?
             SOLAR_OS_NATIVE_DISPLAY_BOARD_PRIMARY : 0U);
}

static int display_info(size_t index, solar_os_native_display_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    solar_os_display_target_t target;
    if (!solar_os_display_get_target(index, &target)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    display_info_copy(out, &target);
    return SOLAR_OS_NATIVE_OK;
}

static int display_find(const char *name, solar_os_native_display_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!name || !*name) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_display_target_t target;
    if (!solar_os_display_find_target(name, &target)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    display_info_copy(out, &target);
    return SOLAR_OS_NATIVE_OK;
}

static int display_get_profile(const char *name, solar_os_terminal_profile_t *profile)
{
    if (!profile) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(profile, 0, sizeof(*profile));
    return solar_os_native_result(solar_os_display_get_terminal_profile(name, profile));
}

static int display_set_profile(const char *name, const solar_os_terminal_profile_t *profile)
{
    return solar_os_native_result(solar_os_sessions_set_display_terminal_profile(name, profile));
}

static int display_set_canvas(const char *name, const solar_os_display_rect_t *canvas)
{
    return solar_os_native_result(solar_os_sessions_set_display_canvas(name, canvas));
}

typedef struct {
    atomic_bool in_operation;
} native_display_resource_t;

/* Both resources embed this state first. The handle registry pins their
 * lifetime during each adapter call. The per-resource guard also protects
 * mutable state from overlapping calls and cleanup during initialization. */
static int display_resource_create(uint32_t kind, native_display_resource_t *state,
                                   solar_os_native_handle_destroy_fn destroy, solar_os_native_handle_t *out)
{
    atomic_init(&state->in_operation, true);
    const solar_os_app_t *owner = solar_os_application_descriptor();
    return owner ? solar_os_native_handle_create_owned(kind, state, destroy, owner, out) :
        solar_os_native_handle_create(kind, state, destroy, out);
}

static int display_resource_enter(solar_os_native_handle_t handle, uint32_t kind, void **out)
{
    native_display_resource_t *state = solar_os_native_handle_acquire(handle, kind);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (atomic_exchange(&state->in_operation, true)) {
        solar_os_native_handle_release(handle);
        return SOLAR_OS_NATIVE_ERROR_BUSY;
    }
    *out = state;
    return SOLAR_OS_NATIVE_OK;
}

static void display_resource_leave(solar_os_native_handle_t handle, native_display_resource_t *state)
{
    atomic_store(&state->in_operation, false);
    solar_os_native_handle_release(handle);
}

typedef struct {
    native_display_resource_t resource;
    char name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    bool started;
    solar_os_display_frame_t frame;
} native_display_export_t;

static int display_export_destroy(void *value)
{
    native_display_export_t *state = value;
    if (atomic_load(&state->resource.in_operation) || state->frame.data) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (state->started) solar_os_display_stop_frame_export(state->name);
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int display_export_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_EXPORT);
}

static int display_export_open(const char *name, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!name || !*name || strlen(name) >= SOLAR_OS_DISPLAY_TARGET_NAME_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_display_export_t *state = solar_os_memory_calloc(1, sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.display.export");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    strcpy(state->name, name);
    int result = display_resource_create(SOLAR_OS_NATIVE_HANDLE_DISPLAY_EXPORT,
        &state->resource, display_export_destroy, out);
    if (result) {
        solar_os_memory_free(state);
        return result;
    }
    result = solar_os_native_result(solar_os_display_start_frame_export(state->name));
    state->started = result == SOLAR_OS_NATIVE_OK;
    atomic_store(&state->resource.in_operation, false);
    if (result) {
        const int close_result = display_export_close(*out);
        if (close_result == SOLAR_OS_NATIVE_OK || close_result == SOLAR_OS_NATIVE_ERROR_NOT_FOUND) *out = 0;
    }
    return result;
}

static int display_frame_acquire(solar_os_native_handle_t handle, solar_os_native_display_frame_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_display_frame_v1_t){0};
    void *resource;
    int result = display_resource_enter(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_EXPORT, &resource);
    if (result) return result;
    native_display_export_t *state = resource;
    if (state->frame.data) result = SOLAR_OS_NATIVE_ERROR_BUSY;
    else if (!state->started) result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    else result = solar_os_native_result(solar_os_display_acquire_frame(state->name, &state->frame));
    if (!result) {
        const solar_os_display_frame_t *frame = &state->frame;
        *out = (solar_os_native_display_frame_v1_t){
            .mono = frame->data, .rgb565 = frame->rgb565_data,
            .mono_size = frame->data_size, .rgb565_size = frame->rgb565_size,
            .frame_id = frame->frame_id, .width = frame->width, .height = frame->height,
            .native_width = frame->native_width, .native_height = frame->native_height,
            .mono_stride = frame->native_stride, .rotation = frame->rotation,
            .black_is_one = frame->black_is_one,
        };
    }
    display_resource_leave(handle, &state->resource);
    return result;
}

static int display_frame_release(solar_os_native_handle_t handle)
{
    void *resource;
    int result = display_resource_enter(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_EXPORT, &resource);
    if (result) return result;
    native_display_export_t *state = resource;
    if (state->frame.data) solar_os_display_release_frame(&state->frame);
    else result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    display_resource_leave(handle, &state->resource);
    return result;
}

static int display_get_mode(const char *name, char *mode, size_t mode_capacity,
                            char *options, size_t options_capacity)
{
    return solar_os_native_result(solar_os_display_get_controller_mode(
        name, mode, mode_capacity, options, options_capacity));
}

static int display_set_mode(const char *name, const char *mode)
{
    return solar_os_native_result(solar_os_display_set_controller_mode(name, mode));
}

static int display_get_theme(solar_os_display_theme_v1_t *theme)
{
    if (!theme) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *theme = (solar_os_display_theme_v1_t){0};
    return solar_os_native_result(solar_os_display_get_theme(theme));
}

static int display_set_theme_color(solar_os_display_theme_role_t role, uint32_t rgb888)
{
    return solar_os_native_result(solar_os_display_set_theme_color(role, rgb888));
}

static const solar_os_native_display_api_v1_t display_api = {
    .abi_version = SOLAR_OS_NATIVE_DISPLAY_ABI,
    .struct_size = sizeof(display_api),
    .count = solar_os_display_target_count,
    .info = display_info,
    .find = display_find,
    .get_profile = display_get_profile,
    .set_profile = display_set_profile,
    .set_canvas = display_set_canvas,
    .export_open = display_export_open,
    .frame_acquire = display_frame_acquire,
    .frame_release = display_frame_release,
    .export_close = display_export_close,
    .get_mode = display_get_mode,
    .set_mode = display_set_mode,
    .get_theme = display_get_theme,
    .set_theme_color = display_set_theme_color,
};
static int display_layout_info(size_t index, solar_os_display_layout_info_t *info)
{
    if (!info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_display_layout_get(index, info) ?
        SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int display_layout_find(solar_os_display_layout_kind_t kind, const char *name,
                               solar_os_display_layout_info_t *info)
{
    return solar_os_native_result(solar_os_display_layout_find(kind, name, info));
}

static int display_layout_create(const solar_os_display_layout_request_v1_t *request,
                                 char *busy_owner, size_t capacity)
{
    return solar_os_native_result(solar_os_sessions_create_display_layout(request, busy_owner, capacity));
}

static int display_layout_remove(solar_os_display_layout_kind_t kind, const char *name)
{
    return solar_os_native_result(solar_os_sessions_remove_display_layout(kind, name));
}

static const solar_os_native_display_layout_api_v1_t display_layout_api = {
    .abi_version = SOLAR_OS_NATIVE_DISPLAY_LAYOUT_ABI,
    .struct_size = sizeof(display_layout_api),
    .count = solar_os_display_layout_count,
    .info = display_layout_info,
    .find = display_layout_find,
    .create = display_layout_create,
    .remove = display_layout_remove,
    .axis_name = solar_os_display_layout_axis_name,
};

typedef struct {
    native_display_resource_t resource;
    char name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
    solar_os_gfx_t *gfx;
    solar_os_frame_presenter_t *frames;
    uint16_t *palette;
    size_t frame_size;
    bool claimed, gfx_borrowed;
    bool frames_ready, frame_buffer_borrowed;
    bool refresh_active, frames_own_refresh;
} native_display_lease_t;

static int display_lease_frames_retire(native_display_lease_t *state)
{
    if (state->frame_buffer_borrowed) return SOLAR_OS_NATIVE_ERROR_BUSY;
    state->frames_ready = false;
    esp_err_t error = solar_os_frame_presenter_deinit(state->frames);
    if (error != ESP_OK) return solar_os_native_result(error);
    state->frames = NULL;
    state->frames_own_refresh = false;
    solar_os_memory_free(state->palette);
    state->palette = NULL;
    return SOLAR_OS_NATIVE_OK;
}

static int display_lease_destroy(void *value)
{
    native_display_lease_t *state = value;
    if (atomic_load(&state->resource.in_operation) || state->gfx_borrowed) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (state->frames) {
        int result = display_lease_frames_retire(state);
        if (result) return result;
    }
    if (state->refresh_active) {
        const esp_err_t error = solar_os_display_set_high_refresh_owned(state->name, state, false, 0);
        if (error != ESP_OK) return solar_os_native_result(error);
        state->refresh_active = false;
    }
    if (state->claimed) {
        const esp_err_t error = solar_os_display_release(state->name, state->owner);
        if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
    }
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int display_lease_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_LEASE);
}

static int display_lease_open(const char *name, const char *label, bool with_gfx,
                              solar_os_native_handle_t *out, char *busy_owner, size_t capacity)
{
    if (busy_owner && capacity) busy_owner[0] = '\0';
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!name || !*name || strnlen(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) == SOLAR_OS_DISPLAY_TARGET_NAME_MAX ||
        !label || !*label) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_display_lease_t *state = solar_os_memory_calloc(1, sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.display.lease");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    strcpy(state->name, name);
    int result = display_resource_create(SOLAR_OS_NATIVE_HANDLE_DISPLAY_LEASE,
        &state->resource, display_lease_destroy, out);
    if (result) {
        solar_os_memory_free(state);
        return result;
    }

    snprintf(state->owner, sizeof(state->owner), "%08lx/%.22s", (unsigned long)*out, label);
    char current_owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX] = {0};
    const esp_err_t error = with_gfx ?
        solar_os_display_open_gfx(state->name, state->owner, &state->gfx, current_owner, sizeof(current_owner)) :
        solar_os_display_claim(state->name, state->owner, current_owner, sizeof(current_owner));
    state->claimed = error == ESP_OK;
    atomic_store(&state->resource.in_operation, false);
    if (error == ESP_OK) return SOLAR_OS_NATIVE_OK;

    if (busy_owner && capacity) strlcpy(busy_owner, current_owner, capacity);
    const int close_result = display_lease_close(*out);
    if (close_result == SOLAR_OS_NATIVE_OK || close_result == SOLAR_OS_NATIVE_ERROR_NOT_FOUND) *out = 0;
    return error == ESP_ERR_INVALID_STATE && current_owner[0] ? SOLAR_OS_NATIVE_ERROR_BUSY :
        solar_os_native_result(error);
}

static int display_lease_gfx_acquire(solar_os_native_handle_t handle, solar_os_gfx_t **out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    void *resource;
    int result = display_resource_enter(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_LEASE, &resource);
    if (result) return result;
    native_display_lease_t *state = resource;
    if (!state->gfx) result = SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    else if (state->gfx_borrowed || state->frames) result = SOLAR_OS_NATIVE_ERROR_BUSY;
    else {
        *out = state->gfx;
        state->gfx_borrowed = true;
    }
    display_resource_leave(handle, &state->resource);
    return result;
}

static int display_lease_gfx_release(solar_os_native_handle_t handle)
{
    void *resource;
    int result = display_resource_enter(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_LEASE, &resource);
    if (result) return result;
    native_display_lease_t *state = resource;
    if (!state->gfx_borrowed) result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    state->gfx_borrowed = false;
    display_resource_leave(handle, &state->resource);
    return result;
}

static int display_lease_set_policy(solar_os_native_handle_t handle, uint32_t mask, uint32_t values)
{
    _Static_assert(SOLAR_OS_NATIVE_DISPLAY_LEASE_BLOCK_TERMINAL == SOLAR_OS_DISPLAY_CLAIM_BLOCK_TERMINAL,
                   "display claim flag");
    _Static_assert(SOLAR_OS_NATIVE_DISPLAY_LEASE_LOCK_ORIENTATION == SOLAR_OS_DISPLAY_CLAIM_LOCK_ORIENTATION,
                   "display claim flag");
    void *resource;
    int result = display_resource_enter(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_LEASE, &resource);
    if (result) return result;
    native_display_lease_t *state = resource;
    result = solar_os_native_result(solar_os_display_set_claim_policy(state->name, state->owner, mask, values));
    display_resource_leave(handle, &state->resource);
    return result;
}

static int display_lease_relax(solar_os_native_handle_t handle, uint32_t seconds,
                              bool (*cancelled)(void *), void *user,
                              solar_os_native_display_maintenance_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_display_maintenance_v1_t){0};
    void *resource;
    int result = display_resource_enter(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_LEASE, &resource);
    if (result) return result;
    native_display_lease_t *state = resource;
    if (state->gfx_borrowed || state->frames) {
        result = SOLAR_OS_NATIVE_ERROR_BUSY;
    } else {
        solar_os_display_relax_result_t maintenance = {0};
        result = solar_os_native_result(solar_os_display_relax(state->name, state->owner,
            seconds, cancelled, user, &maintenance, NULL, 0));
        out->cycles = maintenance.cycles;
        out->cancelled = maintenance.cancelled;
    }
    display_resource_leave(handle, &state->resource);
    return result;
}

static int display_lease_frames_open(solar_os_native_handle_t handle,
                                     const solar_os_frame_presenter_options_v1_t *options)
{
    if (!options || (options->fit != SOLAR_OS_FRAME_FIT_DEFAULT &&
                     options->fit != SOLAR_OS_FRAME_FIT_HEIGHT))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    size_t colors = 0;
    switch (options->format) {
    case SOLAR_OS_DISPLAY_FORMAT_INDEX2: colors = 4; break;
    case SOLAR_OS_DISPLAY_FORMAT_INDEX8: colors = 256; break;
    case SOLAR_OS_DISPLAY_FORMAT_MONO1: case SOLAR_OS_DISPLAY_FORMAT_RGB565: break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    if (colors && (!options->palette_rgb565 || options->palette_size < colors))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    void *resource;
    int result = display_resource_enter(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_LEASE, &resource);
    if (result) return result;
    native_display_lease_t *state = resource;
    if (state->gfx_borrowed || state->frames ||
             (state->refresh_active && options->request_high_refresh)) {
        result = SOLAR_OS_NATIVE_ERROR_BUSY;
    }
    else if (!state->gfx) result = SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    if (!result && colors) {
        state->palette = solar_os_memory_alloc(colors * sizeof(*state->palette),
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.display.palette");
        if (!state->palette) result = SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
        else memcpy(state->palette, options->palette_rgb565, colors * sizeof(*state->palette));
    }
    if (!result) {
        solar_os_frame_presenter_options_v1_t config = *options;
        config.palette_rgb565 = state->palette;
        config.palette_size = colors;
        state->frames_own_refresh = config.request_high_refresh;
        result = solar_os_native_result(solar_os_frame_presenter_init(&state->frames, state->gfx, &config));
        if (result) (void)display_lease_frames_retire(state);
        else {
            state->frame_size = (size_t)config.height * config.stride;
            state->frames_ready = true;
        }
    }
    display_resource_leave(handle, &state->resource);
    return result;
}

static int display_lease_frames_close(solar_os_native_handle_t handle)
{
    void *resource;
    int result = display_resource_enter(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_LEASE, &resource);
    if (result) return result;
    native_display_lease_t *state = resource;
    result = display_lease_frames_retire(state);
    display_resource_leave(handle, &state->resource);
    return result;
}

static int display_lease_frames_enter(solar_os_native_handle_t handle, native_display_lease_t **out)
{
    void *resource;
    int result = display_resource_enter(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_LEASE, &resource);
    if (result) return result;
    native_display_lease_t *state = resource;
    if (!state->frames_ready) {
        display_resource_leave(handle, &state->resource);
        return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    *out = state;
    return SOLAR_OS_NATIVE_OK;
}

static int display_lease_frames_lifecycle(solar_os_native_handle_t handle,
                                         esp_err_t (*change)(solar_os_frame_presenter_t *))
{
    native_display_lease_t *state;
    int result = display_lease_frames_enter(handle, &state);
    if (result) return result;
    result = solar_os_native_result(change(state->frames));
    display_resource_leave(handle, &state->resource);
    return result;
}

static int display_lease_frames_suspend(solar_os_native_handle_t handle)
{ return display_lease_frames_lifecycle(handle, solar_os_frame_presenter_suspend_checked); }

static int display_lease_frames_resume(solar_os_native_handle_t handle)
{ return display_lease_frames_lifecycle(handle, solar_os_frame_presenter_resume); }

static int display_lease_frames_submit(solar_os_native_handle_t handle, const void *data, size_t bytes)
{
    if (!data) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_display_lease_t *state;
    int result = display_lease_frames_enter(handle, &state);
    if (result) return result;
    if (bytes < state->frame_size) result = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    else if (state->frame_buffer_borrowed) result = SOLAR_OS_NATIVE_ERROR_BUSY;
    else if (!solar_os_frame_presenter_submit(state->frames, data, bytes)) result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    display_resource_leave(handle, &state->resource);
    return result;
}

static int display_lease_frames_acquire(solar_os_native_handle_t handle, uint8_t **data, size_t *bytes)
{
    if (!data || !bytes) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *data = NULL;
    *bytes = 0;
    native_display_lease_t *state;
    int result = display_lease_frames_enter(handle, &state);
    if (result) return result;
    if (state->frame_buffer_borrowed) result = SOLAR_OS_NATIVE_ERROR_BUSY;
    else {
        result = solar_os_native_result(solar_os_frame_presenter_acquire(state->frames, data, bytes));
        if (!result) state->frame_buffer_borrowed = true;
    }
    display_resource_leave(handle, &state->resource);
    return result;
}

static int display_lease_frames_exchange(solar_os_native_handle_t handle, uint8_t **data)
{
    native_display_lease_t *state;
    int result = display_lease_frames_enter(handle, &state);
    if (result) return result;
    result = solar_os_native_result(solar_os_frame_presenter_exchange(state->frames, data));
    display_resource_leave(handle, &state->resource);
    return result;
}

static int display_lease_frames_release(solar_os_native_handle_t handle, const uint8_t *data)
{
    native_display_lease_t *state;
    int result = display_lease_frames_enter(handle, &state);
    if (result) return result;
    result = solar_os_native_result(solar_os_frame_presenter_release(state->frames, data));
    if (!result) state->frame_buffer_borrowed = false;
    display_resource_leave(handle, &state->resource);
    return result;
}

static int display_lease_frames_redraw(solar_os_native_handle_t handle)
{
    native_display_lease_t *state;
    int result = display_lease_frames_enter(handle, &state);
    if (result) return result;
    if (!solar_os_frame_presenter_redraw(state->frames)) result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    display_resource_leave(handle, &state->resource);
    return result;
}

static int display_lease_frames_take_stats(solar_os_native_handle_t handle, solar_os_native_frame_presenter_stats_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_frame_presenter_stats_v1_t){0};
    native_display_lease_t *state;
    int result = display_lease_frames_enter(handle, &state);
    if (result) return result;
    solar_os_frame_presenter_stats_t stats;
    solar_os_frame_presenter_take_stats(state->frames, &stats);
    *out = (solar_os_native_frame_presenter_stats_v1_t){.present_us = stats.present_us,
        .presented_frames = stats.presented_frames, .replaced_frames = stats.replaced_frames,
        .last_error = solar_os_native_result(stats.last_error)};
    display_resource_leave(handle, &state->resource);
    return SOLAR_OS_NATIVE_OK;
}

static int display_lease_set_high_refresh(solar_os_native_handle_t handle, uint32_t rate)
{
    if (rate > UINT16_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    void *resource;
    int result = display_resource_enter(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_LEASE, &resource);
    if (result) return result;
    native_display_lease_t *state = resource;
    if (state->frames_own_refresh) {
        result = SOLAR_OS_NATIVE_ERROR_BUSY;
    } else if (!state->claimed) {
        result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    } else if (rate || state->refresh_active) {
        const esp_err_t error = solar_os_display_set_high_refresh_owned(state->name, state, rate != 0, rate);
        result = error == ESP_ERR_INVALID_STATE ? SOLAR_OS_NATIVE_ERROR_BUSY : solar_os_native_result(error);
        if (error == ESP_OK) state->refresh_active = rate != 0;
    }
    display_resource_leave(handle, &state->resource);
    return result;
}

static const solar_os_native_display_lease_api_v1_t display_lease_api = {
    .abi_version = SOLAR_OS_NATIVE_DISPLAY_LEASE_ABI,
    .struct_size = sizeof(display_lease_api),
    .open = display_lease_open,
    .gfx_acquire = display_lease_gfx_acquire,
    .gfx_release = display_lease_gfx_release,
    .set_policy = display_lease_set_policy,
    .close = display_lease_close,
    .relax = display_lease_relax,
    .frames_open = display_lease_frames_open, .frames_suspend = display_lease_frames_suspend,
    .frames_resume = display_lease_frames_resume, .frames_close = display_lease_frames_close,
    .frames_submit = display_lease_frames_submit,
    .frames_acquire = display_lease_frames_acquire, .frames_exchange = display_lease_frames_exchange,
    .frames_release = display_lease_frames_release, .frames_redraw = display_lease_frames_redraw,
    .frames_take_stats = display_lease_frames_take_stats,
    .set_high_refresh = display_lease_set_high_refresh,
};

typedef struct {
    native_display_resource_t resource;
    char target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
} native_display_layer_t;

static int display_layer_clear_owned(const native_display_layer_t *state)
{
    if (!solar_os_display_has_layer(state->target, state->owner)) return SOLAR_OS_NATIVE_OK;
    esp_err_t error = solar_os_display_clear_layer(state->target, state->owner);
    /* A display reconfiguration may clear the layer while we wait for its
     * presentation lock. The old owner has nothing left to retire then. */
    if (error == ESP_ERR_NOT_FOUND || (error == ESP_ERR_INVALID_STATE &&
        !solar_os_display_has_layer(state->target, state->owner))) return SOLAR_OS_NATIVE_OK;
    return solar_os_native_result(error);
}

static int display_layer_destroy(void *value)
{
    native_display_layer_t *state = value;
    if (atomic_load(&state->resource.in_operation)) return SOLAR_OS_NATIVE_ERROR_BUSY;
    int result = display_layer_clear_owned(state);
    if (result) return result;
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int display_layer_open(const char *target, const char *label, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!target || !*target || strlen(target) >= SOLAR_OS_DISPLAY_TARGET_NAME_MAX || !label || !*label)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_display_target_t native_target;
    if (!solar_os_display_find_target(target, &native_target)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (!native_target.u8g2) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    native_display_layer_t *state = solar_os_memory_calloc(1, sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.display.layer");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    strcpy(state->target, target);
    int result = display_resource_create(SOLAR_OS_NATIVE_HANDLE_DISPLAY_LAYER,
        &state->resource, display_layer_destroy, out);
    if (result) {
        solar_os_memory_free(state);
        return result;
    }
    snprintf(state->owner, sizeof(state->owner), "%08lx/%.22s", (unsigned long)*out, label);
    atomic_store(&state->resource.in_operation, false);
    return SOLAR_OS_NATIVE_OK;
}

static int display_layer_show(solar_os_native_handle_t handle, const solar_os_display_rect_t *rect,
                              const uint8_t *pixels, size_t bytes)
{
    if (!rect || !pixels || rect->x < 0 || rect->x > UINT16_MAX || rect->y < 0 || rect->y > UINT16_MAX ||
        rect->width <= 0 || rect->width > UINT16_MAX || rect->height <= 0 || rect->height > UINT16_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (bytes < (((size_t)rect->width + 7U) / 8U) * rect->height) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    void *resource;
    int result = display_resource_enter(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_LAYER, &resource);
    if (result) return result;
    native_display_layer_t *state = resource;
    esp_err_t error = solar_os_display_set_layer(state->target, state->owner, pixels, bytes,
        rect->x, rect->y, rect->width, rect->height);
    display_resource_leave(handle, &state->resource);
    return error == ESP_ERR_INVALID_STATE ? SOLAR_OS_NATIVE_ERROR_BUSY : solar_os_native_result(error);
}

static int display_layer_visible(solar_os_native_handle_t handle, bool *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = false;
    void *resource;
    int result = display_resource_enter(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_LAYER, &resource);
    if (result) return result;
    native_display_layer_t *state = resource;
    *out = solar_os_display_has_layer(state->target, state->owner);
    display_resource_leave(handle, &state->resource);
    return SOLAR_OS_NATIVE_OK;
}

static int display_layer_clear(solar_os_native_handle_t handle)
{
    void *resource;
    int result = display_resource_enter(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_LAYER, &resource);
    if (result) return result;
    native_display_layer_t *state = resource;
    result = display_layer_clear_owned(state);
    display_resource_leave(handle, &state->resource);
    return result;
}

static int display_layer_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_DISPLAY_LAYER);
}

static const solar_os_native_display_layer_api_v1_t display_layer_api = {
    .abi_version = SOLAR_OS_NATIVE_DISPLAY_LAYER_ABI, .struct_size = sizeof(display_layer_api),
    .open = display_layer_open, .show = display_layer_show, .visible = display_layer_visible,
    .clear = display_layer_clear, .close = display_layer_close,
};
#endif

const void *solar_os_native_display_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_display_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
