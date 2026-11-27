#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_gfx.h"
#include "solar_os_m3u.h"
#include "solar_os_native_client_ui.h"

void solar_os_media_transport_button_draw(
    solar_os_gfx_t *gfx,
    int x,
    int y,
    int width,
    int height,
    solar_os_media_transport_icon_t icon,
    bool active);

void solar_os_media_list_layout(int width, int height, solar_os_media_list_layout_t *layout);
void solar_os_media_playlist_draw(solar_os_gfx_t *gfx, int width, int height,
                                   size_t count, size_t cursor, size_t active, size_t *top,
                                   solar_os_m3u_get_fn get, void *user, const char *help);
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
