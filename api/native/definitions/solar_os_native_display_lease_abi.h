#pragma once

#include "solar_os_native_service_abi.h"
#include "solar_os_gfx_types_v1.h"

#define SOLAR_OS_NATIVE_DISPLAY_LEASE_SERVICE "display.lease"
#define SOLAR_OS_NATIVE_DISPLAY_LEASE_ABI 1U

#define SOLAR_OS_NATIVE_DISPLAY_LEASE_BLOCK_TERMINAL (1U << 0)
#define SOLAR_OS_NATIVE_DISPLAY_LEASE_LOCK_ORIENTATION (1U << 1)

typedef struct {
    uint32_t cycles;
    bool cancelled;
} solar_os_native_display_maintenance_v1_t;

typedef enum {
    SOLAR_OS_FRAME_FIT_DEFAULT = 0,
    SOLAR_OS_FRAME_FIT_HEIGHT,
} solar_os_frame_fit_t;

typedef struct {
    solar_os_display_format_t format;
    uint16_t width, height, stride;
    const uint16_t *palette_rgb565;
    size_t palette_size;
    uint16_t preferred_fps;
    solar_os_frame_fit_t fit;
    /* Dither indexed or RGB565 input on targets that present MONO1. */
    bool allow_mono_fallback;
    bool request_high_refresh;
    bool reverse_direct_palette;
    /* Preserve colors on direct targets and convert literal mono luma. */
    bool literal_direct_palette;
    bool literal_mono_palette;
    bool clear_background_on_resume;
    uint8_t background_index;
    uint16_t background_rgb565;
    /* Fit against the full panel, including a negative canvas origin. */
    bool fullscreen;
} solar_os_frame_presenter_options_v1_t;

typedef struct {
    uint64_t present_us;
    uint32_t presented_frames, replaced_frames;
    int last_error;
} solar_os_native_frame_presenter_stats_v1_t;

/* Opening claims one physical or logical target. Each lease has a distinct
 * owner even when labels match. with_gfx also opens its drawing surface.
 * The surface is borrowed for use with gfx until gfx_release. Draw on one
 * task at a time. Release the view before closing the lease. Close and app
 * cleanup return BUSY while the view is borrowed. set_policy changes only
 * the selected flags. Closing restores terminal drawing and orientation. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*open)(const char *name, const char *label, bool with_gfx,
                solar_os_native_handle_t *lease, char *busy_owner, size_t capacity);
    int (*gfx_acquire)(solar_os_native_handle_t lease, solar_os_gfx_t **gfx);
    int (*gfx_release)(solar_os_native_handle_t lease);
    int (*set_policy)(solar_os_native_handle_t lease, uint32_t mask, uint32_t values);
    int (*close)(solar_os_native_handle_t lease);
    /* Run the target's recovery cycles for 1–86400 seconds. A borrowed
     * drawing view must be released first. The callback runs on the
     * calling task and is borrowed only for this call. */
    int (*relax)(solar_os_native_handle_t lease, uint32_t seconds,
                 bool (*cancelled)(void *user), void *user,
                 solar_os_native_display_maintenance_v1_t *result);
    /* One frame presenter can own the lease's graphics surface. Release any
     * drawing view first. Options and palette are copied during open. A failed
     * open can require frames_close before the next open. */
    int (*frames_open)(solar_os_native_handle_t lease, const solar_os_frame_presenter_options_v1_t *options);
    int (*frames_suspend)(solar_os_native_handle_t lease);
    int (*frames_resume)(solar_os_native_handle_t lease);
    int (*frames_close)(solar_os_native_handle_t lease);
    /* The presenter owns a copied frame. A newer queued frame replaces an
     * older one if the target has yet to begin presenting it. */
    int (*frames_submit)(solar_os_native_handle_t lease, const void *pixels, size_t bytes);
    /* A zero-copy writer borrows one buffer until frames_release. Exchange
     * submits it and hands back another. The submitted buffer is read-only
     * until the next exchange, then belongs solely to the presenter. */
    int (*frames_acquire)(solar_os_native_handle_t lease, uint8_t **pixels, size_t *bytes);
    int (*frames_exchange)(solar_os_native_handle_t lease, uint8_t **pixels);
    int (*frames_release)(solar_os_native_handle_t lease, const uint8_t *pixels);
    int (*frames_redraw)(solar_os_native_handle_t lease);
    int (*frames_take_stats)(solar_os_native_handle_t lease, solar_os_native_frame_presenter_stats_v1_t *stats);
    /* Take the target's refresh override at a supported rate in tenths of
     * a hertz. Zero restores the prior policy. Close also restores it and
     * retains the lease on failure for retry. A frame presenter that requests
     * its own high-refresh mode is mutually exclusive with this setting. */
    int (*set_high_refresh)(solar_os_native_handle_t lease, uint32_t rate_tenths);
} solar_os_native_display_lease_api_v1_t;
