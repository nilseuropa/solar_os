#pragma once

#include "solar_os_native_service_abi.h"
#include "solar_os_gfx_types_v1.h"
#include "solar_os_terminal_types_v1.h"

#define SOLAR_OS_NATIVE_DISPLAY_SERVICE "display"
#define SOLAR_OS_NATIVE_DISPLAY_ABI 1U

#define SOLAR_OS_NATIVE_DISPLAY_READY (1U << 0)
#define SOLAR_OS_NATIVE_DISPLAY_ROUND (1U << 1)
#define SOLAR_OS_NATIVE_DISPLAY_EPAPER (1U << 2)
#define SOLAR_OS_NATIVE_DISPLAY_BLACK_IS_ONE (1U << 3)
#define SOLAR_OS_NATIVE_DISPLAY_BRIGHTNESS (1U << 4)
#define SOLAR_OS_NATIVE_DISPLAY_PREFER_MONO (1U << 5)
#define SOLAR_OS_NATIVE_DISPLAY_BOARD_PRIMARY (1U << 6)

typedef struct {
    char name[16], source[12], driver[20], controller[20], role[16], owner[32];
    solar_os_display_rect_t panel, canvas;
    uint16_t width, height, native_width, native_height;
    uint16_t preferred_stream_fps, presentation_stack_bytes;
    uint32_t max_stream_pixels_per_second;
    uint32_t flags, surface_formats, frame_formats;
} solar_os_native_display_info_v1_t;

typedef struct {
    const uint8_t *mono, *rgb565;
    size_t mono_size, rgb565_size;
    uint32_t frame_id;
    uint16_t width, height, native_width, native_height, mono_stride;
    solar_os_display_rotation_t rotation;
    bool black_is_one;
} solar_os_native_display_frame_v1_t;

/* A target snapshot owns its values and contains no driver references.
 * Indices may change as displays attach and detach. Layout updates are
 * synchronous on the session owner and affect all terminals on the target.
 * Call them on that task or on a task the owner is free to wait for.
 * Changes are temporary. NULL canvas restores the target's default rectangle.
 * Coordinates are in the panel's oriented, scaled pixel space. */
typedef struct {
    uint32_t abi_version, struct_size;
    size_t (*count)(void);
    int (*info)(size_t index, solar_os_native_display_info_v1_t *info);
    int (*find)(const char *name, solar_os_native_display_info_v1_t *info);
    int (*get_profile)(const char *name, solar_os_terminal_profile_t *profile);
    int (*set_profile)(const char *name, const solar_os_terminal_profile_t *profile);
    int (*set_canvas)(const char *name, const solar_os_display_rect_t *canvas);
    /* One exporter per target. Each handle can borrow one immutable frame.
     * Release the frame before closing the exporter. Close returns BUSY
     * while a frame is borrowed, including during application cleanup.
     * Pixels are in native orientation. Monochrome bytes represent 8 vertical
     * pixels, least significant bit first, one row per 8-pixel band and
     * mono_stride bytes per row. Optional RGB565 is row-major, high byte
     * first, with no row padding. Frame views expire at frame_release. */
    int (*export_open)(const char *name, solar_os_native_handle_t *handle);
    int (*frame_acquire)(solar_os_native_handle_t handle, solar_os_native_display_frame_v1_t *frame);
    int (*frame_release)(solar_os_native_handle_t handle);
    int (*export_close)(solar_os_native_handle_t handle);
    /* Copy the controller description and the space-separated options
     * advertised by that driver. Either output may be NULL. A short buffer
     * returns INVALID_SIZE. Calls may fail during a maintenance cycle.
     * set_mode accepts a driver option or a driver-supported value assignment. */
    int (*get_mode)(const char *name, char *mode, size_t mode_capacity,
                    char *options, size_t options_capacity);
    int (*set_mode)(const char *name, const char *mode);
    /* Shared terminal theme. A change updates every attached color target.
     * Color values are 24-bit RGB. Changes use the saved shell settings. */
    int (*get_theme)(solar_os_display_theme_v1_t *theme);
    int (*set_theme_color)(solar_os_display_theme_role_t role, uint32_t rgb888);
} solar_os_native_display_api_v1_t;
