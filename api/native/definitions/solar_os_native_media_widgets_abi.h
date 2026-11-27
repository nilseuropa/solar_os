#pragma once
#include "solar_os_native_service_abi.h"
#include "solar_os_gfx_types_v1.h"
#include "solar_os_native_media_types_abi.h"

#define SOLAR_OS_NATIVE_MEDIA_WIDGETS_SERVICE "ui.media"
#define SOLAR_OS_NATIVE_MEDIA_WIDGETS_ABI 1U

/* Common media views and models. Borrow gfx on its owning task, with the
 * selected surface's dimensions. Coordinates follow gfx's signed 16-bit
 * contract. Strings, layout output, animation state and playlist context are
 * caller-owned. Playlist lookup runs synchronously on this calling task and
 * retains nothing. Supply top, and get when count is nonzero. Draw calls use
 * the existing Core kernels without allocating objects or extra buffers. */
typedef struct {
    uint32_t abi_version, struct_size;
    void (*transport_button_draw)(solar_os_gfx_t *, int x, int y, int width, int height,
        solar_os_media_transport_icon_t icon, bool active);
    void (*list_layout)(int width, int height, solar_os_media_list_layout_t *layout);
    void (*playlist_draw)(solar_os_gfx_t *, int width, int height, size_t count,
        size_t cursor, size_t active, size_t *top, solar_os_m3u_get_fn get, void *user, const char *help);
    void (*player_layout)(int width, int height, bool seek, solar_os_media_player_layout_t *layout);
    void (*player_header_draw)(solar_os_gfx_t *, int width, const char *title, const char *tabs);
    void (*player_status_draw)(solar_os_gfx_t *, int width, int height, const char *status);
    void (*player_controls_draw)(solar_os_gfx_t *, int width, int height, const char *title,
        const char *status, uint8_t volume, bool seek, solar_os_media_transport_icon_t middle);
    void (*controls_draw)(solar_os_gfx_t *, int width, int height, const char *title,
        const char *status, uint8_t volume, const solar_os_media_transport_icon_t *icons,
        const bool *active, int count);
    int (*player_button_at)(int width, int height, bool seek, int x, int y);
    void (*cassette_reset)(solar_os_cassette_widget_t *state);
    void (*cassette_update)(solar_os_cassette_widget_t *state, bool playing,
        uint32_t elapsed_ms, uint32_t total_ms, uint32_t now_ms);
    void (*cassette_draw)(solar_os_cassette_widget_t *state, solar_os_gfx_t *,
        int x, int y, int width, int height);
} solar_os_native_media_widgets_api_v1_t;

#define SOLAR_OS_NATIVE_SIGNAL_WIDGET_SERVICE "ui.signal"
#define SOLAR_OS_NATIVE_SIGNAL_WIDGET_ABI 1U
#define SOLAR_OS_SIGNAL_WAVEFORM 1U
#define SOLAR_OS_SIGNAL_SPECTRUM 2U

/* Submit copies interleaved PCM. Borrow gfx on the surface's owning task.
 * Serialize reset and draw. Close can be retried when operations are active. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*create)(uint32_t kind, size_t samples, solar_os_native_handle_t *widget);
    int (*close)(solar_os_native_handle_t widget);
    int (*reset)(solar_os_native_handle_t widget);
    int (*submit_s16)(solar_os_native_handle_t widget, const int16_t *samples, size_t frames, uint8_t channels);
    int (*draw)(solar_os_native_handle_t widget, solar_os_gfx_t *gfx, int x, int y, int width, int height);
} solar_os_native_signal_widget_api_v1_t;
