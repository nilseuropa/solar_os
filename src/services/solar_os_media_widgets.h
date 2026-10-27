#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_gfx.h"

typedef struct solar_os_cassette_widget solar_os_cassette_widget_t;

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

void solar_os_media_transport_button_draw(
    solar_os_gfx_t *gfx,
    int x,
    int y,
    int width,
    int height,
    solar_os_media_transport_icon_t icon,
    bool active);

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
void solar_os_media_list_layout(int width, int height, solar_os_media_list_layout_t *layout);
/* Draw a single line clipped to the supplied pixel width. */
void solar_os_media_text_draw(solar_os_gfx_t *gfx, int x, int baseline,
                              int width, const char *text);
void solar_os_media_dialog_draw(solar_os_gfx_t *gfx, int width, int height,
                                const char *title, const char *input, const char *help);

/* Allocation-free common chrome; decoding, clocks and transport remain app-owned. */
void solar_os_media_player_layout(int width, int height, bool seek,
                                   solar_os_media_player_layout_t *layout);
void solar_os_media_player_header_draw(solar_os_gfx_t *gfx, int width,
                                        const char *title, const char *tabs);
void solar_os_media_player_status_draw(solar_os_gfx_t *gfx, int width, int height,
                                        const char *status);
void solar_os_media_player_controls_draw(solar_os_gfx_t *gfx, int width, int height,
                                          const char *title, const char *status,
                                          uint8_t volume, bool seek,
                                          solar_os_media_transport_icon_t middle);
void solar_os_media_controls_draw(solar_os_gfx_t *gfx, int width, int height,
                                  const char *title, const char *status, uint8_t volume,
                                  const solar_os_media_transport_icon_t *icons,
                                  const bool *active, int count);
/* Returns -1 outside a button, including the gaps. */
int solar_os_media_player_button_at(int width, int height, bool seek, int x, int y);

esp_err_t solar_os_cassette_widget_create(solar_os_cassette_widget_t **widget);
void solar_os_cassette_widget_destroy(solar_os_cassette_widget_t *widget);
void solar_os_cassette_widget_reset(solar_os_cassette_widget_t *widget);
void solar_os_cassette_widget_update(solar_os_cassette_widget_t *widget,
                                     bool playing,
                                     uint32_t elapsed_ms,
                                     uint32_t total_ms,
                                     uint32_t now_ms);
void solar_os_cassette_widget_draw(solar_os_cassette_widget_t *widget,
                                   solar_os_gfx_t *gfx,
                                   int x,
                                   int y,
                                   int width,
                                   int height);
