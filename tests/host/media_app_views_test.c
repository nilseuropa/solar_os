#include "solar_os_media_widgets.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#define RECORDER_HEADER_HEIGHT 28
#define RECORDER_FOOTER_HEIGHT 20
#define RECORDER_SETUP_ROWS 8
#define RECORDER_FILENAME_MAX 64
#define WEBRADIO_GUI_INPUT_MAX 256
#define SOLAR_OS_WEBRADIO_STATION_NAME_MAX 64
static solar_os_gfx_color_t current_color;
static unsigned selected_texts;
struct solar_os_gfx {
    int width, height;
    solar_os_gfx_font_t font;
};
size_t solar_os_gfx_width(const solar_os_gfx_t *g) { return g->width; }
size_t solar_os_gfx_height(const solar_os_gfx_t *g) { return g->height; }
void solar_os_gfx_set_font(solar_os_gfx_t *g, solar_os_gfx_font_t f) {
    g->font = f;
}
void solar_os_gfx_set_color(solar_os_gfx_t *g, solar_os_gfx_color_t c) {
    (void)g;
    current_color = c;
}
size_t solar_os_gfx_text_width(solar_os_gfx_t *g, const char *s) {
    return strlen(s) * (g->font == SOLAR_OS_GFX_FONT_BITMAP_8 ? 5 : 7);
}
void solar_os_gfx_text(solar_os_gfx_t *g, int x, int y, const char *s) {
    if (current_color == SOLAR_OS_GFX_COLOR_WHITE && s[0]) selected_texts++;
    assert(x >= 0 && y > 0 &&
           x + (int)solar_os_gfx_text_width(g, s) <= g->width && y < g->height);
}
void solar_os_gfx_rect(solar_os_gfx_t *g, int x, int y, int w, int h) {
    assert(x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= g->width &&
           y + h <= g->height);
}
void solar_os_gfx_fill_rect(solar_os_gfx_t *g, int x, int y, int w, int h) {
    solar_os_gfx_rect(g, x, y, w, h);
}
void solar_os_gfx_line(solar_os_gfx_t *g, int x, int y, int x1, int y1) {
    assert(x >= 0 && y >= 0 && x < g->width && x1 >= 0 && x1 < g->width &&
           y < g->height && y1 >= 0 && y1 < g->height);
}
void solar_os_gfx_fill_circle(solar_os_gfx_t *g, int x, int y, int r) {
    solar_os_gfx_rect(g, x - r, y - r, 2 * r + 1, 2 * r + 1);
}
void solar_os_gfx_fill_polygon(solar_os_gfx_t *g, const solar_os_gfx_point_t *p,
                               size_t n) {
    for (size_t i = 0; i < n; i++)
        solar_os_gfx_rect(g, p[i].x, p[i].y, 1, 1);
}
static void visualizer(void *widget, solar_os_gfx_t *g, int x, int y, int w,
                       int h) {
    (void)widget;
    solar_os_gfx_rect(g, x, y, w, h);
}
void solar_os_cassette_widget_draw(solar_os_cassette_widget_t *p,
                                   solar_os_gfx_t *g, int x, int y, int w,
                                   int h) {
    visualizer(p, g, x, y, w, h);
}
static void solar_os_oscilloscope_widget_draw(void *p, solar_os_gfx_t *g, int x,
                                              int y, int w, int h) {
    visualizer(p, g, x, y, w, h);
}
static void solar_os_spectrum_widget_draw(void *p, solar_os_gfx_t *g, int x,
                                          int y, int w, int h) {
    visualizer(p, g, x, y, w, h);
}
enum {
    PLAYER_TAB_PLAY,
    PLAYER_TAB_LIST,
    PLAYER_VISUALIZER_CASSETTE = 0,
    PLAYER_VISUALIZER_SCOPE,
    PLAYER_VISUALIZER_SPECTRUM
};
enum {
    RECORDER_IDLE,
    RECORDER_RECORDING,
    RECORDER_MONITORING,
    RECORDER_PAUSED,
    RECORDER_PLAYING,
    RECORDER_ERROR
};
enum {
    RECORDER_VISUALIZER_CASSETTE,
    RECORDER_VISUALIZER_SCOPE,
    RECORDER_VISUALIZER_SPECTRUM
};
enum {
    WEBRADIO_TAB_PLAYER,
    WEBRADIO_TAB_CHANNELS,
    WEBRADIO_VISUALIZER_SCOPE = 0,
    WEBRADIO_VISUALIZER_SPECTRUM
};
enum {
    WEBRADIO_DIALOG_NONE,
    WEBRADIO_DIALOG_ADD_NAME,
    WEBRADIO_DIALOG_ADD_URL,
    WEBRADIO_DIALOG_EDIT_NAME,
    WEBRADIO_DIALOG_EDIT_URL
};
typedef enum {
    WEBRADIO_PLAYBACK_IDLE,
    WEBRADIO_PLAYBACK_PLAYING,
    WEBRADIO_PLAYBACK_ERROR
} webradio_playback_state_t;
typedef struct {
    bool is_directory;
    char name[192];
} solar_os_storage_browser_entry_t;
typedef struct {
    char path[192];
} solar_os_player_track_t;
static size_t browser_cursor;
static size_t solar_os_storage_browser_count(void *p) {
    (void)p;
    return 10;
}
static size_t solar_os_storage_browser_cursor(void *p) {
    (void)p;
    return browser_cursor;
}
static bool
solar_os_storage_browser_entry(void *p, size_t i,
                               solar_os_storage_browser_entry_t *e) {
    (void)p;
    e->is_directory = i % 2;
    snprintf(e->name, sizeof(e->name), "directory-or-long-track-name-%zu.wav",
             i);
    return i < 10;
}
static bool solar_os_player_playlist_get(size_t i, solar_os_player_track_t *t) {
    snprintf(t->path, sizeof(t->path), "track-%zu.wav", i);
    return i < 10;
}
static struct {
    int tab, visualizer;
    void *cassette, *scope, *spectrum, *task, *browser;
    char active_path[192];
    bool paused, browsing;
    uint8_t volume;
    size_t cursor, top, track_count, active_index;
} player;
static struct {
    int visualizer, tab, operation_state;
    void *cassette, *scope, *spectrum, *browser;
    uint8_t volume, channels, bits_per_sample;
    uint32_t sample_rate, elapsed_ms;
    bool monitor_enabled;
    size_t setup_cursor, browser_top;
    char active_path[192], filename[65], edit_filename[65];
} recorder;
static struct {
    int tab, visualizer, dialog;
    void *scope, *spectrum;
    uint8_t volume;
    bool paused;
    size_t cursor, top, station_count;
    char dialog_input[256], ui_message[96];
    struct {
        char name[64];
    } stations[10];
} webradio;
static const char *player_basename(const char *s) { return s; }
static const char *recorder_basename(const char *s) { return s; }
static void player_progress_text(char *s, size_t n) {
    snprintf(s, n, "PLAYING 00:12 / 01:23");
}
static void recorder_format_time(uint32_t ms, char *s, size_t n) {
    (void)ms;
    snprintf(s, n, "00:12");
}
static const char *recorder_state_name(void) { return "RECORDING"; }
static void recorder_setup_value(size_t i, char *label, size_t l, char *value,
                                 size_t v) {
    snprintf(label, l, "Setting %zu", i);
    snprintf(value, v, "value-with-a-very-long-directory-or-stream-name");
}
static void webradio_snapshot_status(webradio_playback_state_t *s, char *msg,
                                     size_t m, char *name, size_t n, void *a,
                                     void *b, void *c) {
    (void)a;
    (void)b;
    (void)c;
    *s = WEBRADIO_PLAYBACK_PLAYING;
    if (msg)
        snprintf(msg, m, "Playing");
    if (name)
        snprintf(name, n, "Station with a long name");
}
static void webradio_progress_text(webradio_playback_state_t s, const char *msg,
                                   char *out, size_t n) {
    (void)s;
    (void)msg;
    snprintf(out, n, "PLAYING 00:12");
}
typedef struct {
    uint32_t screen_width, screen_height, viewport_x, viewport_y,
        viewport_width, viewport_height;
    bool fullscreen;
} mpeg_player_t;
#include "media_app_views.inc"
int main(void) {
    const int screens[][2] = {{240, 64},  {128, 64},  {240, 135},
                              {240, 240}, {384, 168}, {400, 300}};
    for (unsigned screen = 0; screen < sizeof(screens) / sizeof(*screens);
         screen++) {
        solar_os_gfx_t g = {.width = screens[screen][0],
                            .height = screens[screen][1]};
        int w = g.width, h = g.height;
        player.volume = recorder.volume = webradio.volume = 50;
        player.track_count = 10;
        recorder.sample_rate = 48000;
        recorder.channels = 2;
        recorder.bits_per_sample = 16;
        for (int v = 0; v < 3; v++) {
            player.visualizer = recorder.visualizer = v;
            player_draw_header(&g, w);
            player_render_play(&g, w, h);
            recorder_draw_header(&g, w);
            recorder_render_record_graphics(&g, w, h);
            recorder_draw_progress(&g, w, h, true);
        }
        for (int browsing = 0; browsing < 2; browsing++) {
            player.browsing = browsing;
            for (size_t cursor = 0; cursor < 10; cursor++) {
                player.cursor = browser_cursor = cursor;
                selected_texts = 0;
                player_render_list(&g, w, h);
                assert(selected_texts);
                selected_texts = 0;
                recorder_render_browser_graphics(&g, w, h);
                assert(selected_texts);
                assert(player.top <= cursor && recorder.browser_top <= cursor);
            }
        }
        for (size_t cursor = 0; cursor < 8; cursor++) {
            recorder.setup_cursor = cursor;
            selected_texts = 0;
            recorder_render_setup_graphics(&g, w, h);
            assert(selected_texts);
        }
        memset(recorder.edit_filename, 'a', 64);
        recorder.edit_filename[64] = 0;
        recorder_draw_filename_dialog(&g, w, h);
        webradio.station_count = 10;
        for (unsigned i = 0; i < 10; i++)
            snprintf(webradio.stations[i].name, 64, "A long station name %u",
                     i);
        for (int v = 0; v < 2; v++) {
            webradio.visualizer = v;
            webradio_draw_graphics_header(&g, w);
            webradio_render_player(&g, w, h);
        }
        for (size_t cursor = 0; cursor < 10; cursor++) {
            webradio.cursor = cursor;
            selected_texts = 0;
            webradio_render_channels(&g, w, h, WEBRADIO_PLAYBACK_PLAYING, "");
            assert(selected_texts);
            assert(webradio.top <= cursor);
        }
        memset(webradio.dialog_input, 'x', 255);
        webradio.dialog_input[255] = 0;
        for (int dialog = 1; dialog <= 4; dialog++) {
            webradio.dialog = dialog;
            webradio_draw_dialog(&g, w, h);
        }
        mpeg_player_t p = {.screen_width = w, .screen_height = h};
        viewport(&p);
        assert(p.viewport_width > 0 && p.viewport_height > 0 &&
               p.viewport_y + p.viewport_height < (uint32_t)h);
        p.fullscreen = true;
        viewport(&p);
        assert(p.viewport_width == (uint32_t)w &&
               p.viewport_height == (uint32_t)h && !p.viewport_y);
    }
    puts("Recorder, Player, WebRadio and VPlay views fit small and standard "
         "displays");
}
