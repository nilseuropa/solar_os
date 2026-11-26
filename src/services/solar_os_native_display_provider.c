#include "solar_os_native_display_provider.h"

#include <stdatomic.h>
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_display_provider_abi.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_display.h"
#include "solar_os_memory.h"
#include "solar_os_native.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
#include "solar_os_native_runtime.h"
#endif

#define DISPLAY_PROVIDER_KIND SOLAR_OS_NATIVE_HANDLE_DISPLAY_PROVIDER

typedef struct {
    u8g2_t canvas;
    u8x8_display_info_t info;
    solar_os_native_display_provider_callbacks_v1_t callbacks;
    solar_os_native_handle_t library;
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    void *driver_pin;
#endif
    atomic_bool opening;
    bool registered;
    char name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    uint8_t mono[];
} native_display_provider_t;

static uint8_t display_callback(u8x8_t *display, uint8_t message, uint8_t arg, void *data)
{
    (void)data;
    native_display_provider_t *state = (void *)((uint8_t *)display -
        offsetof(native_display_provider_t, canvas) - offsetof(u8g2_t, u8x8));
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(display, &state->info);
    } else if (message == U8X8_MSG_DISPLAY_REFRESH) {
        const uint16_t stride = (uint16_t)state->info.tile_width * 8U;
        const solar_os_native_display_mono_view_v1_t view = {
            .pixels = u8g2_GetBufferPtr(&state->canvas),
            .size = (size_t)stride * state->info.tile_height,
            .width = state->info.pixel_width, .height = state->info.pixel_height, .stride = stride,
        };
        return state->callbacks.present_mono(state->callbacks.user, &view) == SOLAR_OS_NATIVE_OK;
    } else if (message == U8X8_MSG_DISPLAY_SET_POWER_SAVE && state->callbacks.sleep) {
        return state->callbacks.sleep(state->callbacks.user, arg != 0) == SOLAR_OS_NATIVE_OK;
    }
    return 1;
}

static esp_err_t present_surface(void *opaque, const solar_os_display_surface_t *surface)
{
    native_display_provider_t *state = opaque;
    return solar_os_native_callback_result(state->callbacks.present_surface(state->callbacks.user, surface));
}

static esp_err_t present_frame(void *opaque, const solar_os_display_raster_t *frame)
{
    native_display_provider_t *state = opaque;
    return solar_os_native_callback_result(state->callbacks.present_frame(state->callbacks.user, frame));
}

static esp_err_t set_colors(void *opaque, uint32_t foreground, uint32_t background)
{
    native_display_provider_t *state = opaque;
    return solar_os_native_callback_result(state->callbacks.set_colors(state->callbacks.user, foreground, background));
}

static const char *get_mode(const void *opaque)
{
    const native_display_provider_t *state = opaque;
    return state->callbacks.get_mode(state->callbacks.user);
}

static const char *mode_choices(const void *opaque)
{
    const native_display_provider_t *state = opaque;
    return state->callbacks.mode_choices(state->callbacks.user);
}

static esp_err_t set_mode(void *opaque, const char *mode)
{
    native_display_provider_t *state = opaque;
    return solar_os_native_callback_result(state->callbacks.set_mode(state->callbacks.user, mode));
}

static esp_err_t relax(void *opaque, solar_os_display_relax_phase_t phase)
{
    native_display_provider_t *state = opaque;
    return solar_os_native_callback_result(state->callbacks.relax(state->callbacks.user, phase));
}

static esp_err_t present_power_state(void *opaque)
{
    native_display_provider_t *state = opaque;
    return solar_os_native_callback_result(state->callbacks.present_power_state(state->callbacks.user));
}

static int provider_destroy(void *opaque)
{
    native_display_provider_t *state = opaque;
    if (atomic_load(&state->opening)) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (state->registered) {
        const esp_err_t err = solar_os_display_unregister_target_owned(state->name, &state->canvas);
        if (err == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return solar_os_native_result(err);
        state->registered = false;
    }
    if (state->library) {
        const int result = solar_os_native_library_release(state->library);
        if (result && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (state->driver_pin) solar_os_native_driver_unpin(state->driver_pin);
#endif
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static bool provider_valid(const solar_os_native_display_provider_v1_t *in)
{
    if (!in || !in->name[0] || !in->driver[0] || !in->width || !in->height ||
        in->width > UINT8_MAX * 8U || in->height > UINT8_MAX * 8U ||
        !memchr(in->name, 0, sizeof(in->name)) || !memchr(in->controller, 0, sizeof(in->controller)) ||
        !memchr(in->role, 0, sizeof(in->role)) || !memchr(in->driver, 0, sizeof(in->driver))) return false;
    const uint32_t flags = SOLAR_OS_NATIVE_DISPLAY_ROUND | SOLAR_OS_NATIVE_DISPLAY_EPAPER |
        SOLAR_OS_NATIVE_DISPLAY_BLACK_IS_ONE | SOLAR_OS_NATIVE_DISPLAY_PREFER_MONO;
    const uint32_t formats = SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT | SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT |
        SOLAR_OS_DISPLAY_FORMAT_INDEX2_BIT | SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT;
    const solar_os_native_display_provider_callbacks_v1_t *ops = &in->callbacks;
    return !(in->flags & ~flags) && !(in->frame_formats & ~formats) &&
        !(in->surface_formats & ~SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT) && ops->present_mono &&
        (!!ops->present_surface == !!in->surface_formats) && (!!ops->present_frame == !!in->frame_formats) &&
        (!ops->present_frame || in->preferred_stream_fps) &&
        (!!ops->get_mode == !!ops->mode_choices) && (!!ops->get_mode == !!ops->set_mode);
}

static esp_err_t provider_publish(native_display_provider_t *state,
    const solar_os_native_display_provider_v1_t *in)
{
    u8g2_SetupDisplay(&state->canvas, display_callback, u8x8_cad_empty, u8x8_dummy_cb, u8x8_dummy_cb);
    u8g2_SetupBuffer(&state->canvas, state->mono, state->info.tile_height, u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    u8g2_InitDisplay(&state->canvas);
    const solar_os_native_display_provider_callbacks_v1_t *ops = &state->callbacks;
    solar_os_display_target_t target = {
        .source = "native", .width = in->width, .height = in->height,
        .native_width = in->width, .native_height = in->height,
        .round = (in->flags & SOLAR_OS_NATIVE_DISPLAY_ROUND) != 0,
        .epaper = (in->flags & SOLAR_OS_NATIVE_DISPLAY_EPAPER) != 0,
        .black_is_one = (in->flags & SOLAR_OS_NATIVE_DISPLAY_BLACK_IS_ONE) != 0,
        .prefer_mono = (in->flags & SOLAR_OS_NATIVE_DISPLAY_PREFER_MONO) != 0,
        .ready = true, .u8g2 = &state->canvas, .surface_formats = in->surface_formats,
        .frame_formats = in->frame_formats, .preferred_stream_fps = in->preferred_stream_fps,
        .presentation_stack_bytes = in->presentation_stack_bytes,
        .max_stream_pixels_per_second = in->max_stream_pixels_per_second,
        .present_surface = ops->present_surface ? present_surface : NULL,
        .surface_context = ops->present_surface ? state : NULL,
        .present_frame = ops->present_frame ? present_frame : NULL,
        .frame_context = ops->present_frame ? state : NULL,
        .set_colors = ops->set_colors ? set_colors : NULL,
        .color_context = state,
        .controller_mode = ops->get_mode ? get_mode : NULL,
        .controller_mode_values = ops->mode_choices ? mode_choices : NULL,
        .set_controller_mode = ops->set_mode ? set_mode : NULL,
        .controller_context = ops->get_mode ? state : NULL,
        .relax = ops->relax ? relax : NULL, .relax_context = state,
        .present_power_state = ops->present_power_state ? present_power_state : NULL,
        .power_state_context = state,
    };
    strcpy(target.name, state->name);
    strcpy(target.driver, in->driver);
    strcpy(target.controller, in->controller);
    strcpy(target.role, in->role);
    return solar_os_display_register_target(&target);
}

static int provider_register(solar_os_native_handle_t library,
    const solar_os_native_display_provider_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!provider_valid(in)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#if !SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (!library) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#endif
    const uint8_t columns = (in->width + 7U) / 8U, rows = (in->height + 7U) / 8U;
    native_display_provider_t *state = solar_os_memory_calloc(1,
        sizeof(*state) + (size_t)columns * rows * 8U, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "display.provider");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    strcpy(state->name, in->name);
    state->callbacks = in->callbacks;
    state->info = (u8x8_display_info_t){.tile_width = columns, .tile_height = rows,
        .pixel_width = in->width, .pixel_height = in->height, .chip_disable_level = 1};
    atomic_init(&state->opening, true);
    solar_os_native_handle_t id;
    int result = solar_os_native_handle_create(DISPLAY_PROVIDER_KIND, state, provider_destroy, &id);
    if (result) {
        solar_os_memory_free(state);
        return result;
    }
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (!library) {
        state->driver_pin = solar_os_native_driver_pin();
        result = state->driver_pin ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    } else
#endif
    result = solar_os_native_library_retain(library, &state->library);
    if (!result) result = solar_os_native_result(provider_publish(state, in));
    state->registered = result == SOLAR_OS_NATIVE_OK;
    atomic_store(&state->opening, false);
    if (!result) *out = id;
    else if (solar_os_native_handle_close_kind(id, DISPLAY_PROVIDER_KIND)) *out = id;
    return result;
}

static int provider_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, DISPLAY_PROVIDER_KIND); }

static const solar_os_native_display_provider_api_v1_t display_provider_api = {
    .abi_version = SOLAR_OS_NATIVE_DISPLAY_PROVIDER_ABI, .struct_size = sizeof(display_provider_api),
    .register_display = provider_register, .close = provider_close,
};
#endif

const void *solar_os_native_display_provider_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_display_provider_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
