#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_M3U_TITLE_MAX 96U
#define SOLAR_OS_M3U_PATH_MAX 320U
typedef struct {
    /* NUL-terminated strings. A lookup fills both fields and duration. */
    char path[SOLAR_OS_M3U_PATH_MAX];
    char title[SOLAR_OS_M3U_TITLE_MAX];
    int32_t duration; /* Seconds; -1 means unknown, never a seek estimate. */
} solar_os_m3u_entry_t;
/* Synchronous row lookup. Fill the caller's record; no pointers are retained. */
typedef bool (*solar_os_m3u_get_fn)(size_t, solar_os_m3u_entry_t *, void *);

typedef enum {
    SOLAR_OS_MEDIA_TRANSPORT_PREVIOUS,
    SOLAR_OS_MEDIA_TRANSPORT_PLAY,
    SOLAR_OS_MEDIA_TRANSPORT_PAUSE,
    SOLAR_OS_MEDIA_TRANSPORT_STOP,
    SOLAR_OS_MEDIA_TRANSPORT_RECORD,
    SOLAR_OS_MEDIA_TRANSPORT_NEXT,
    SOLAR_OS_MEDIA_TRANSPORT_REWIND,
    SOLAR_OS_MEDIA_TRANSPORT_FORWARD,
} solar_os_media_transport_icon_t;

#define SOLAR_OS_MEDIA_PLAYER_HEADER_HEIGHT 28
#define SOLAR_OS_MEDIA_PLAYER_CONTROLS_HEIGHT 78
typedef struct {
    int view_x, view_y, view_width, view_height;
    int controls_y, title_y, status_y, volume_y, button_y, button_width, button_count;
    int header_height, button_height, button_gap;
    bool compact;
} solar_os_media_player_layout_t;
typedef struct {
    int top, row_height, baseline, footer_height, visible;
    bool compact;
} solar_os_media_list_layout_t;

/* Caller-owned animation model. Embed in application state or initialize
 * with {0}; it owns no resources, callbacks, buffers or rendering references. */
typedef struct {
    uint8_t phase;
    bool playing;
    uint32_t elapsed_ms, total_ms, last_tick_ms;
} solar_os_cassette_widget_t;
