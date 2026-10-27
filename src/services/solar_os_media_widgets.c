#include "solar_os_media_widgets.h"

#include <string.h>
#include <stdio.h>

#include "solar_os_memory.h"

struct solar_os_cassette_widget {
    uint8_t phase;
    bool playing;
    uint32_t elapsed_ms;
    uint32_t total_ms;
    uint32_t last_tick_ms;
};

static void media_transport_triangle(solar_os_gfx_t *gfx,
                                     int left,
                                     int top,
                                     int width,
                                     int height,
                                     bool points_right)
{
    const solar_os_gfx_point_t points[] = {
        {points_right ? left : left + width - 1, top},
        {points_right ? left : left + width - 1, top + height - 1},
        {points_right ? left + width - 1 : left, top + (height - 1) / 2},
    };
    solar_os_gfx_fill_polygon(gfx, points, 3U);
}

void solar_os_media_transport_button_draw(
    solar_os_gfx_t *gfx,
    int x,
    int y,
    int width,
    int height,
    solar_os_media_transport_icon_t icon,
    bool active)
{
    if (gfx == NULL || width < 12 || height < 12) {
        return;
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    if (active) {
        solar_os_gfx_fill_rect(gfx, x, y, width, height);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    } else {
        solar_os_gfx_rect(gfx, x, y, width, height);
    }

    int size = height - 10;
    if (size > width - 12) size = width - 12;
    if (size < 6) size = 6;
    const int cx = x + width / 2;
    const int cy = y + height / 2;
    const int left = cx - size / 2;
    const int top = cy - size / 2;
    const int bar = size / 4 > 2 ? size / 4 : 2;

    switch (icon) {
    case SOLAR_OS_MEDIA_TRANSPORT_REWIND:
    case SOLAR_OS_MEDIA_TRANSPORT_FORWARD: {
        const int half = size / 2;
        const bool right = icon == SOLAR_OS_MEDIA_TRANSPORT_FORWARD;
        media_transport_triangle(gfx, cx - half - 1, top, half, size, right);
        media_transport_triangle(gfx, cx + 1, top, half, size, right);
        break;
    }
    case SOLAR_OS_MEDIA_TRANSPORT_PREVIOUS:
        solar_os_gfx_fill_rect(gfx, left, top, bar, size);
        media_transport_triangle(gfx, left + bar + 1, top, size, size, false);
        break;
    case SOLAR_OS_MEDIA_TRANSPORT_PLAY:
        media_transport_triangle(gfx, left, top, size, size, true);
        break;
    case SOLAR_OS_MEDIA_TRANSPORT_PAUSE:
        solar_os_gfx_fill_rect(gfx, left, top, bar, size);
        solar_os_gfx_fill_rect(gfx, left + size - bar, top, bar, size);
        break;
    case SOLAR_OS_MEDIA_TRANSPORT_STOP:
        solar_os_gfx_fill_rect(gfx, left, top, size, size);
        break;
    case SOLAR_OS_MEDIA_TRANSPORT_RECORD:
        solar_os_gfx_fill_circle(gfx, cx, cy, size / 2);
        break;
    case SOLAR_OS_MEDIA_TRANSPORT_NEXT:
        media_transport_triangle(gfx, left - bar - 1, top, size, size, true);
        solar_os_gfx_fill_rect(gfx, left + size, top, bar, size);
        break;
    default:
        break;
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
}

void solar_os_media_player_layout(int width, int height, bool seek,
                                  solar_os_media_player_layout_t *layout) {
    *layout = (solar_os_media_player_layout_t){
        .view_x = 5,
        .view_y = SOLAR_OS_MEDIA_PLAYER_HEADER_HEIGHT + 4,
        .view_width = width - 10,
        .view_height = height - SOLAR_OS_MEDIA_PLAYER_HEADER_HEIGHT -
                       SOLAR_OS_MEDIA_PLAYER_CONTROLS_HEIGHT - 8,
        .controls_y = height - SOLAR_OS_MEDIA_PLAYER_CONTROLS_HEIGHT,
        .title_y = height - 63,
        .status_y = height - 48,
        .volume_y = height - 43,
        .button_y = height - 25,
        .button_count = seek ? 5 : 3,
        .header_height = SOLAR_OS_MEDIA_PLAYER_HEADER_HEIGHT,
        .button_height = 21,
        .button_gap = 5,
    };
    layout->compact = height < 180 || width < 240;
    if (layout->compact) {
        layout->header_height = 10;
        layout->button_height = 12;
        layout->button_gap = 2;
        layout->controls_y = height - (height < 56   ? 25
                                       : height < 96 ? 34
                                                     : 44);
        layout->title_y = height < 56 ? -1 : layout->controls_y + 8;
        layout->button_y = height - layout->button_height - 2;
        layout->volume_y =
            height < 96 || width < 112 ? -1 : layout->button_y - 10;
        layout->status_y = layout->button_y - (layout->volume_y < 0 ? 3 : 13);
        layout->view_x = 2;
        layout->view_y = layout->header_height + 2;
        layout->view_width = width - 4;
        layout->view_height = layout->controls_y - layout->view_y - 2;
    }
    if (layout->view_width < 0)
        layout->view_width = 0;
    if (layout->view_height < 0)
        layout->view_height = 0;
    layout->button_width =
        (width - layout->button_gap * (layout->button_count + 1)) /
        layout->button_count;
}

static void media_clip_text(solar_os_gfx_t *gfx, char *out, size_t capacity,
                            int width, const char *text) {
    snprintf(out, capacity, "%s", text ? text : "");
    while (out[0] && solar_os_gfx_text_width(gfx, out) >
                         (size_t)(width > 0 ? width : 0)) {
        size_t n = strlen(out) - 1;
        while (n && ((unsigned char)out[n] & 0xc0) == 0x80)
            n--;
        out[n] = 0;
    }
}
void solar_os_media_text_draw(solar_os_gfx_t *gfx, int x, int baseline,
                              int width, const char *text) {
    char clipped[192];
    media_clip_text(gfx, clipped, sizeof(clipped), width, text);
    if (clipped[0])
        solar_os_gfx_text(gfx, x, baseline, clipped);
}
void solar_os_media_list_layout(int width, int height,
                                solar_os_media_list_layout_t *l) {
    solar_os_media_player_layout_t player;
    solar_os_media_player_layout(width, height, false, &player);
    *l = (solar_os_media_list_layout_t){
        .compact = player.compact,
        .top = player.header_height + (player.compact ? 2 : 5),
        .row_height = player.compact ? 10 : 24,
        .baseline = player.compact ? 7 : 17,
        .footer_height = player.compact ? 10 : 20,
    };
    l->visible = (height - l->footer_height - l->top) / l->row_height;
    if (l->visible < 0)
        l->visible = 0;
}
void solar_os_media_dialog_draw(solar_os_gfx_t *gfx, int width, int height,
                                const char *title, const char *input,
                                const char *help) {
    bool compact = height < 160 || width < 240;
    int w = width - (compact ? 4 : 24), h = compact ? height - 4 : 76;
    if (h > 60 && compact)
        h = 60;
    int x = (width - w) / 2, y = (height - h) / 2;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, w, h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, w, h);
    solar_os_gfx_set_font(gfx, compact ? SOLAR_OS_GFX_FONT_BITMAP_8
                                       : SOLAR_OS_GFX_FONT_BOLD_14);
    solar_os_media_text_draw(gfx, x + 4, y + (compact ? 10 : 20), w - 8, title);
    solar_os_gfx_set_font(gfx, compact ? SOLAR_OS_GFX_FONT_BITMAP_8
                                       : SOLAR_OS_GFX_FONT_MONO_12);
    const char *visible = input;
    while (*visible &&
           solar_os_gfx_text_width(gfx, visible) > (size_t)(w - 8)) {
        visible++;
        while (((unsigned char)*visible & 0xc0) == 0x80)
            visible++;
    }
    solar_os_media_text_draw(gfx, x + 4, y + h / 2 + 4, w - 8, visible);
    solar_os_gfx_set_font(gfx, compact ? SOLAR_OS_GFX_FONT_BITMAP_8
                                       : SOLAR_OS_GFX_FONT_SMALL);
    solar_os_media_text_draw(gfx, x + 4, y + h - 5, w - 8, help);
}

static void media_player_centered(solar_os_gfx_t *gfx, int width, int baseline,
                                  const char *text) {
    char clipped[192];
    media_clip_text(gfx, clipped, sizeof(clipped), width > 8 ? width - 8 : 0,
                    text);
    solar_os_gfx_text(gfx,
                      (width - (int)solar_os_gfx_text_width(gfx, clipped)) / 2,
                      baseline, clipped);
}

void solar_os_media_player_header_draw(solar_os_gfx_t *gfx, int width,
                                       const char *title, const char *tabs) {
    solar_os_media_player_layout_t layout;
    solar_os_media_player_layout(width, (int)solar_os_gfx_height(gfx), false,
                                 &layout);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, width, layout.header_height);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, layout.compact ? SOLAR_OS_GFX_FONT_BITMAP_8
                                              : SOLAR_OS_GFX_FONT_MONO_12);
    int tab_width = tabs ? (int)solar_os_gfx_text_width(gfx, tabs) : 0;
    if (tab_width > width * 2 / 3)
        tab_width = width * 2 / 3;
    if (tab_width)
        solar_os_media_text_draw(gfx, width - tab_width - 3,
                                 layout.compact ? 8 : 18, tab_width, tabs);
    solar_os_gfx_set_font(gfx, layout.compact ? SOLAR_OS_GFX_FONT_BITMAP_8
                                              : SOLAR_OS_GFX_FONT_BOLD_16);
    solar_os_media_text_draw(gfx, 3, layout.compact ? 8 : 19,
                             width - tab_width - 9, title);
}

void solar_os_media_player_status_draw(solar_os_gfx_t *gfx, int width,
                                       int height, const char *status) {
    solar_os_media_player_layout_t layout;
    solar_os_media_player_layout(width, height, false, &layout);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    /* Cover the 12 px font's accents/descenders without erasing the tighter
     * title row above or the volume bar below. */
    solar_os_gfx_fill_rect(gfx, 1, layout.status_y - (layout.compact ? 7 : 11),
                           width - 2, layout.compact ? 9 : 15);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, layout.compact ? SOLAR_OS_GFX_FONT_BITMAP_8
                                              : SOLAR_OS_GFX_FONT_MONO_12);
    media_player_centered(gfx, width, layout.status_y, status);
}

void solar_os_media_controls_draw(solar_os_gfx_t *gfx, int width, int height,
                                  const char *title, const char *status,
                                  uint8_t volume,
                                  const solar_os_media_transport_icon_t *icons,
                                  const bool *active, int count) {
    if (!icons || count < 1 || count > 5)
        return;
    solar_os_media_player_layout_t layout;
    solar_os_media_player_layout(width, height, count == 5, &layout);
    layout.button_count = count;
    layout.button_width = (width - layout.button_gap * (count + 1)) / count;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, 0, layout.controls_y, width,
                           height - layout.controls_y);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, 0, layout.controls_y, width - 1, layout.controls_y);
    solar_os_gfx_set_font(gfx, layout.compact ? SOLAR_OS_GFX_FONT_BITMAP_8
                                              : SOLAR_OS_GFX_FONT_BOLD_16);
    if (layout.title_y >= 0) {
        if (layout.volume_y < 0) {
            solar_os_media_text_draw(gfx, 2, layout.title_y, width - 30, title);
            char percent[8];
            snprintf(percent, sizeof(percent), "%u%%",
                     volume > 100 ? 100 : volume);
            solar_os_gfx_text(
                gfx, width - (int)solar_os_gfx_text_width(gfx, percent) - 2,
                layout.title_y, percent);
        } else
            media_player_centered(gfx, width, layout.title_y, title);
    }
    solar_os_media_player_status_draw(gfx, width, height, status);
    if (layout.volume_y >= 0) {
        solar_os_gfx_set_font(gfx, layout.compact ? SOLAR_OS_GFX_FONT_BITMAP_8
                                                  : SOLAR_OS_GFX_FONT_SMALL);
        const int volume_width = width / 2,
                  volume_x = (width - volume_width) / 2;
        const int bar_height = layout.compact ? 6 : 10;
        if (volume > 100)
            volume = 100;
        solar_os_gfx_text(gfx, volume_x - 25, layout.volume_y + bar_height - 1,
                          "VOL");
        solar_os_gfx_rect(gfx, volume_x, layout.volume_y, volume_width,
                          bar_height);
        int filled = (volume_width - 4) * volume / 100;
        if (filled > 0)
            solar_os_gfx_fill_rect(gfx, volume_x + 2, layout.volume_y + 2,
                                   filled, bar_height - 4);
        char percent[8];
        snprintf(percent, sizeof(percent), "%u%%", volume);
        solar_os_media_text_draw(gfx, volume_x + volume_width + 3,
                                 layout.volume_y + bar_height - 1,
                                 width - volume_x - volume_width - 3, percent);
    }
    for (int i = 0; i < layout.button_count; i++) {
        solar_os_media_transport_button_draw(
            gfx, layout.button_gap * (i + 1) + layout.button_width * i,
            layout.button_y, layout.button_width, layout.button_height,
            icons[i], active && active[i]);
    }
}

void solar_os_media_player_controls_draw(
    solar_os_gfx_t *gfx, int width, int height, const char *title,
    const char *status, uint8_t volume, bool seek,
    solar_os_media_transport_icon_t middle) {
    const solar_os_media_transport_icon_t icons[] = {
        SOLAR_OS_MEDIA_TRANSPORT_PREVIOUS, SOLAR_OS_MEDIA_TRANSPORT_REWIND,
        middle, SOLAR_OS_MEDIA_TRANSPORT_FORWARD,
        SOLAR_OS_MEDIA_TRANSPORT_NEXT};
    const solar_os_media_transport_icon_t short_icons[] = {
        SOLAR_OS_MEDIA_TRANSPORT_PREVIOUS, middle,
        SOLAR_OS_MEDIA_TRANSPORT_NEXT};
    solar_os_media_controls_draw(gfx, width, height, title, status, volume,
                                 seek ? icons : short_icons, NULL,
                                 seek ? 5 : 3);
}

int solar_os_media_player_button_at(int width, int height, bool seek, int x,
                                    int y) {
    solar_os_media_player_layout_t layout;
    solar_os_media_player_layout(width, height, seek, &layout);
    if (y < layout.button_y || y >= layout.button_y + layout.button_height)
        return -1;
    for (int i = 0; i < layout.button_count; i++) {
        int left = layout.button_gap * (i + 1) + layout.button_width * i;
        if (x >= left && x < left + layout.button_width)
            return i;
    }
    return -1;
}

esp_err_t solar_os_cassette_widget_create(solar_os_cassette_widget_t **out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = solar_os_memory_calloc(1U, sizeof(**out),
                                  SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                  "widget.cassette");
    return *out != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void solar_os_cassette_widget_destroy(solar_os_cassette_widget_t *widget)
{
    solar_os_memory_free(widget);
}

void solar_os_cassette_widget_reset(solar_os_cassette_widget_t *widget)
{
    if (widget != NULL) {
        memset(widget, 0, sizeof(*widget));
    }
}

void solar_os_cassette_widget_update(solar_os_cassette_widget_t *widget,
                                     bool playing,
                                     uint32_t elapsed_ms,
                                     uint32_t total_ms,
                                     uint32_t now_ms)
{
    if (widget == NULL) {
        return;
    }
    if (playing && (!widget->playing || now_ms - widget->last_tick_ms >= 90U)) {
        widget->phase = (uint8_t)((widget->phase + 1U) & 7U);
        widget->last_tick_ms = now_ms;
    }
    widget->playing = playing;
    widget->elapsed_ms = elapsed_ms;
    widget->total_ms = total_ms;
}

static void cassette_spokes(solar_os_gfx_t *gfx, int x, int y, int radius,
                            uint8_t phase)
{
    static const int8_t directions[8][2] = {
        {0, -8}, {6, -6}, {8, 0}, {6, 6},
        {0, 8}, {-6, 6}, {-8, 0}, {-6, -6},
    };
    for (uint8_t i = 0U; i < 4U; i++) {
        const uint8_t direction = (uint8_t)((phase + i * 2U) & 7U);
        solar_os_gfx_line(gfx, x, y,
                          x + directions[direction][0] * radius / 8,
                          y + directions[direction][1] * radius / 8);
    }
}

static void cassette_fill_capsule(solar_os_gfx_t *gfx,
                                  int x,
                                  int y,
                                  int width,
                                  int height)
{
    const int radius = height / 2;
    if (width <= height || radius <= 0) {
        solar_os_gfx_fill_rect(gfx, x, y, width, height);
        return;
    }
    solar_os_gfx_fill_rect(gfx, x + radius, y, width - height, height);
    solar_os_gfx_fill_circle(gfx, x + radius, y + radius, radius);
    solar_os_gfx_fill_circle(gfx, x + width - radius - 1, y + radius, radius);
}

static void cassette_draw_reel(solar_os_gfx_t *gfx,
                               int x,
                               int y,
                               int radius,
                               uint8_t phase)
{
    static const int8_t directions[8][2] = {
        {0, -8}, {6, -6}, {8, 0}, {6, 6},
        {0, 8}, {-6, 6}, {-8, 0}, {-6, -6},
    };

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_circle(gfx, x, y, radius);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    const int tooth_radius = radius > 11 ? 2 : 1;
    const int tooth_distance = radius - tooth_radius;
    /* Four cut-outs make a 45-degree phase change visible.  Eight identical
     * cut-outs occupied every phase position and therefore looked stationary. */
    for (uint8_t i = 0U; i < 4U; i++) {
        const uint8_t direction = (uint8_t)((phase + i * 2U) & 7U);
        const int tx = x + directions[direction][0] * tooth_distance / 8;
        const int ty = y + directions[direction][1] * tooth_distance / 8;
        solar_os_gfx_fill_circle(gfx, tx, ty, tooth_radius);
    }
    solar_os_gfx_fill_circle(gfx, x, y, radius / 3);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    cassette_spokes(gfx, x, y, radius / 3, phase);
}

void solar_os_cassette_widget_draw(solar_os_cassette_widget_t *widget,
                                   solar_os_gfx_t *gfx,
                                   int x,
                                   int y,
                                   int width,
                                   int height)
{
    if (widget == NULL || gfx == NULL || width < 80 || height < 50) {
        return;
    }
    int cassette_height = height * 9 / 10;
    int cassette_width = cassette_height * 3 / 2;
    if (cassette_width > width * 4 / 5) {
        cassette_width = width * 4 / 5;
        cassette_height = cassette_width * 2 / 3;
    }
    const int left = x + (width - cassette_width) / 2;
    const int top = y + (height - cassette_height) / 2;
    const int corner = cassette_height / 24 > 3 ? cassette_height / 24 : 3;
    const int window_x = left + cassette_width * 13 / 100;
    const int window_y = top + cassette_height * 27 / 100;
    const int window_width = cassette_width * 74 / 100;
    const int window_height = cassette_height * 36 / 100;
    const int cy = window_y + window_height / 2;
    const int left_hub = left + cassette_width * 29 / 100;
    const int right_hub = left + cassette_width * 71 / 100;
    const int maximum = window_height * 38 / 100;
    const int minimum = maximum * 4 / 5;
    uint32_t progress = 500U;
    if (widget->total_ms > 0U) {
        progress = widget->elapsed_ms >= widget->total_ms ? 1000U :
            (uint32_t)(((uint64_t)widget->elapsed_ms * 1000U) /
                       widget->total_ms);
    }
    const int supply = maximum - (maximum - minimum) * (int)progress / 1000;
    const int takeup = minimum + (maximum - minimum) * (int)progress / 1000;

    /* Solid shell with the softly clipped corners of the reference cassette. */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, left + corner, top,
                           cassette_width - 2 * corner, cassette_height);
    solar_os_gfx_fill_rect(gfx, left, top + corner,
                           cassette_width, cassette_height - 2 * corner);
    solar_os_gfx_fill_circle(gfx, left + corner, top + corner, corner);
    solar_os_gfx_fill_circle(gfx, left + cassette_width - corner - 1,
                             top + corner, corner);
    solar_os_gfx_fill_circle(gfx, left + corner,
                             top + cassette_height - corner - 1, corner);
    solar_os_gfx_fill_circle(gfx, left + cassette_width - corner - 1,
                             top + cassette_height - corner - 1, corner);

    /* Four recessed shell screws. */
    const int screw_inset = corner + 1;
    const int screw_radius = corner > 4 ? 2 : 1;
    const int screw_x[2] = {left + screw_inset,
                            left + cassette_width - screw_inset - 1};
    const int screw_y[2] = {top + screw_inset,
                            top + cassette_height - screw_inset - 1};
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    for (size_t sy = 0U; sy < 2U; sy++) {
        for (size_t sx = 0U; sx < 2U; sx++) {
            solar_os_gfx_fill_circle(gfx, screw_x[sx], screw_y[sy],
                                     screw_radius);
        }
    }

    /* White-edged tape window, dark interior, reels and center apertures. */
    cassette_fill_capsule(gfx, window_x, window_y,
                          window_width, window_height);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    cassette_fill_capsule(gfx, window_x + 2, window_y + 2,
                          window_width - 4, window_height - 4);

    const int bridge_x = left_hub + maximum;
    const int bridge_width = right_hub - left_hub - 2 * maximum;
    if (bridge_width > 6) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_rect(gfx, bridge_x, cy - maximum / 2,
                               bridge_width, maximum);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        const int split = bridge_width / 7 > 2 ? bridge_width / 7 : 2;
        solar_os_gfx_fill_rect(gfx, bridge_x + 2, cy - maximum / 2 + 2,
                               (bridge_width - split - 4) / 2,
                               maximum - 4);
        solar_os_gfx_fill_rect(gfx,
                               bridge_x + (bridge_width + split) / 2,
                               cy - maximum / 2 + 2,
                               (bridge_width - split - 4) / 2,
                               maximum - 4);
    }
    cassette_draw_reel(gfx, left_hub, cy, supply, widget->phase);
    cassette_draw_reel(gfx, right_hub, cy, takeup, widget->phase);

    /* Lower tape-guide plate and its five characteristic openings. */
    const int plate_top = top + cassette_height * 82 / 100;
    const solar_os_gfx_point_t outer_plate[] = {
        {left + cassette_width * 27 / 100, top + cassette_height - 1},
        {left + cassette_width * 30 / 100, plate_top},
        {left + cassette_width * 70 / 100, plate_top},
        {left + cassette_width * 74 / 100, top + cassette_height - 1},
    };
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_polygon(gfx, outer_plate,
                              sizeof(outer_plate) / sizeof(outer_plate[0]));
    const solar_os_gfx_point_t inner_plate[] = {
        {left + cassette_width * 29 / 100, top + cassette_height - 1},
        {left + cassette_width * 32 / 100, plate_top + 2},
        {left + cassette_width * 68 / 100, plate_top + 2},
        {left + cassette_width * 72 / 100, top + cassette_height - 1},
    };
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_polygon(gfx, inner_plate,
                              sizeof(inner_plate) / sizeof(inner_plate[0]));

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    const int hole_radius = cassette_height / 34 > 1 ?
        cassette_height / 34 : 2;
    solar_os_gfx_fill_circle(gfx, left + cassette_width * 34 / 100,
                             plate_top + cassette_height * 9 / 100,
                             hole_radius);
    solar_os_gfx_fill_circle(gfx, left + cassette_width * 66 / 100,
                             plate_top + cassette_height * 9 / 100,
                             hole_radius);
    solar_os_gfx_fill_circle(gfx, left + cassette_width / 2,
                             plate_top + cassette_height * 4 / 100,
                             hole_radius > 2 ? 2 : 1);
    const int square = hole_radius * 2;
    solar_os_gfx_fill_rect(gfx, left + cassette_width * 40 / 100,
                           plate_top + cassette_height * 8 / 100,
                           square, square);
    solar_os_gfx_fill_rect(gfx, left + cassette_width * 58 / 100 - square,
                           plate_top + cassette_height * 8 / 100,
                           square, square);
}
