#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_gfx.h"
#include "solar_os_media_widgets.h"

#define HEADER_HEIGHT 28
typedef struct {
    bool fullscreen, stopped, paused, suspended, audio_running, audio_eof, seeking;
    unsigned screen_width, screen_height;
    int64_t displayed_second, sink_at, sink_time, sink_quantum, pause_start, wall_start;
    uint64_t sink_frames;
    double start_time;
    struct { bool audio; } info;
} mpeg_player_t;
struct solar_os_gfx { int width, height; };
size_t solar_os_gfx_height(const solar_os_gfx_t *gfx) { return gfx->height; }
static solar_os_gfx_font_t probe_font;
static bool check_bounds;
static int64_t now;
static unsigned draws, rectangles, polygons;
static int glyph_top, glyph_bottom;
static char label[192];
static int last_rect_y, last_rect_height;
typedef enum { WEBRADIO_PLAYBACK_IDLE, WEBRADIO_PLAYBACK_CONNECTING,
    WEBRADIO_PLAYBACK_BUFFERING, WEBRADIO_PLAYBACK_PLAYING,
    WEBRADIO_PLAYBACK_RECONNECTING, WEBRADIO_PLAYBACK_ERROR } webradio_playback_state_t;
static struct {
    uint64_t played_frames;
    uint32_t output_rate;
    bool paused, stop_requested, task_done, redraw;
    void *task;
} webradio;
typedef struct {
    struct { uint32_t sample_rate; } output_format;
    bool playback_started;
} webradio_worker_t;
static void webradio_player_state(bool playing, void *user)
{ ((webradio_worker_t *)user)->playback_started = playing; }
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
static void webradio_publish_visualizer(const int16_t *samples, size_t count, uint8_t channels)
{ (void)samples; (void)count; (void)channels; }
static int64_t esp_timer_get_time(void) { return now; }
void solar_os_gfx_set_color(solar_os_gfx_t *gfx, solar_os_gfx_color_t color)
{ (void)gfx; (void)color; }
void solar_os_gfx_set_font(solar_os_gfx_t *gfx, solar_os_gfx_font_t font)
{ (void)gfx; /* Font metrics used by the geometry probe. */
  probe_font = font; }
size_t solar_os_gfx_text_width(solar_os_gfx_t *gfx, const char *text)
{ (void)gfx; return strlen(text) * (probe_font == SOLAR_OS_GFX_FONT_BITMAP_8 ? 5 : 7); }
void solar_os_gfx_text(solar_os_gfx_t *gfx, int x, int baseline, const char *text)
{
    assert(x >= 0 && baseline > 0);
    if (check_bounds) {
        assert(x + (int)solar_os_gfx_text_width(gfx, text) <= gfx->width);
        assert(baseline < gfx->height);
        assert(baseline >= (probe_font == SOLAR_OS_GFX_FONT_BITMAP_8 ? 7 : 11));
    }
    draws++; snprintf(label, sizeof(label), "%s", text);
}
void solar_os_gfx_rect(solar_os_gfx_t *gfx, int x, int y, int width, int height)
{ if (check_bounds) assert(x >= 0 && y >= 0 && width > 0 && height > 0 &&
    x + width <= gfx->width && y + height <= gfx->height); }
void solar_os_gfx_fill_rect(solar_os_gfx_t *gfx, int x, int y, int width, int height)
{
    (void)gfx; assert(x >= 0 && y >= 0 && width > 0 && height > 0);
    if (check_bounds) assert(x + width <= gfx->width && y + height <= gfx->height);
    rectangles++;
    last_rect_y = y; last_rect_height = height;
    if (y < glyph_top) glyph_top = y;
    if (y + height - 1 > glyph_bottom) glyph_bottom = y + height - 1;
}
void solar_os_gfx_fill_polygon(solar_os_gfx_t *gfx, const solar_os_gfx_point_t *points, size_t count)
{
    (void)gfx; assert(count == 3); polygons++;
    for (size_t i = 0; i < count; i++) {
        if (points[i].y < glyph_top) glyph_top = points[i].y;
        if (points[i].y > glyph_bottom) glyph_bottom = points[i].y;
    }
}
void solar_os_gfx_fill_circle(solar_os_gfx_t *gfx, int x, int y, int radius)
{ (void)gfx; (void)x; (void)y; (void)radius; }
void solar_os_gfx_line(solar_os_gfx_t *gfx, int x, int y, int x1, int y1)
{ (void)gfx; assert(x >= 0 && y >= 0 && x1 >= x && y1 >= y); }
#include "media_transport_ui.inc"

int main(void)
{
    struct solar_os_gfx gfx = {240, 240};
    mpeg_player_t p = {.screen_width = 240, .screen_height = 240};
    assert(draw_play_time(&p, &gfx, 0, true) && strcmp(label, "PLAYING 00:00") == 0);
    assert(!draw_play_time(&p, &gfx, 999999, false));
    assert(draw_play_time(&p, &gfx, 125999999, false) && strcmp(label, "PLAYING 02:05") == 0);
    assert(!draw_play_time(&p, &gfx, 125999999, false));
    assert(draw_play_time(&p, &gfx, 3599000000LL, false) && strcmp(label, "PLAYING 59:59") == 0);
    assert(draw_play_time(&p, &gfx, 3600000000LL, false) && strcmp(label, "PLAYING 60:00") == 0);
    assert(draw_play_time(&p, &gfx, -1, false) && strcmp(label, "PLAYING 00:00") == 0);
    p.fullscreen = true;
    unsigned before = rectangles;
    assert(!draw_play_time(&p, &gfx, 20000000, true) && rectangles == before);
    p.fullscreen = false;
    assert(draw_play_time(&p, &gfx, 12000000, true) && strcmp(label, "PLAYING 00:12") == 0);
    p.stopped = true;
    assert(!draw_play_time(&p, &gfx, 90000000, false));
    assert(draw_play_time(&p, &gfx, 90000000, true) && strcmp(label, "STOPPED 00:12") == 0);
    p.stopped = false;
    now = 5000000; p.wall_start = 1000000;
    assert(clock_locked(&p) == 4000000);
    p.paused = true; p.pause_start = 4000000;
    assert(clock_locked(&p) == 3000000);
    p.info.audio = true; p.sink_time = 2500000; p.sink_at = 4900000; p.sink_quantum = 50000;
    assert(clock_locked(&p) == 2500000);
    p.paused = false; p.audio_running = true;
    assert(clock_locked(&p) == 2550000); /* Audio clock cannot outrun buffered samples. */
    assert(draw_play_time(&p, &gfx, clock_locked(&p), false) && strcmp(label, "PLAYING 00:02") == 0);
    p.paused = true;
    assert(draw_play_time(&p, &gfx, clock_locked(&p), true) && strcmp(label, "PAUSED 00:02") == 0);
    p.seeking = true;
    assert(draw_play_time(&p, &gfx, 30000000, true) && strcmp(label, "SEEKING 00:30") == 0);
    p.paused = p.seeking = false;
    const int screens[][2] = {{240, 240}, {320, 240}, {400, 240}, {480, 320}, {800, 480}};
    for (size_t s = 0; s < sizeof(screens) / sizeof(*screens); s++) {
        int w = screens[s][0], h = screens[s][1];
        for (unsigned seek = 0; seek < 2; seek++) {
            solar_os_media_player_layout_t layout;
            solar_os_media_player_layout(w, h, seek, &layout);
            assert(layout.view_y + layout.view_height < layout.controls_y);
            assert(layout.title_y < layout.status_y && layout.status_y + 3 < layout.volume_y);
            assert(layout.title_y - layout.controls_y >= 15); /* Full accented title above separator. */
            assert(layout.status_y - layout.title_y == 15);
            assert(layout.status_y - 11 > layout.title_y + 3); /* Dynamic clear preserves descenders. */
            assert(layout.button_y - (layout.volume_y + 10) == 8);
            assert(layout.button_y == h - 25 && layout.button_y + 21 < h);
            assert(layout.view_height > h - 28 - 88 - 8); /* Larger than old VPlay viewport. */
            assert(solar_os_media_player_button_at(w, h, seek, w / 2, layout.button_y + 10) == layout.button_count / 2);
            for (int i = 0; i < layout.button_count; i++) {
                int x = 5 * (i + 1) + layout.button_width * i;
                assert(solar_os_media_player_button_at(w, h, seek, x, layout.button_y) == i);
                assert(solar_os_media_player_button_at(w, h, seek, x + layout.button_width - 1, layout.button_y + 20) == i);
                assert(solar_os_media_player_button_at(w, h, seek, x - 1, layout.button_y) == -1);
                assert(solar_os_media_player_button_at(w, h, seek, x, layout.button_y - 1) == -1);
                assert(solar_os_media_player_button_at(w, h, seek, x, layout.button_y + 21) == -1);
            }
            solar_os_media_player_header_draw(&gfx, w, "VPlay", "");
            solar_os_media_player_controls_draw(&gfx, w, h, "a very long filename or station name",
                "PAUSED 00:12", 50, seek, SOLAR_OS_MEDIA_TRANSPORT_PAUSE);
            solar_os_media_player_status_draw(&gfx, w, h, "PLAYING 00:13");
            assert(last_rect_y == layout.status_y - 11 && last_rect_height == 15);
            assert(last_rect_y >= layout.controls_y); /* Dynamic time never touches video. */
        }
    }
    const int small_screens[][2] = {{240, 64}, {128, 64}, {96, 48}, {84, 128}, {240, 135}, {384, 168}};
    for (size_t screen = 0; screen < sizeof(small_screens) / sizeof(*small_screens); screen++) {
        int w = small_screens[screen][0], h = small_screens[screen][1];
        gfx.width = w; gfx.height = h; check_bounds = true;
        for (unsigned seek = 0; seek < 2; seek++) {
            solar_os_media_player_layout_t l;
            solar_os_media_player_layout(w, h, seek, &l);
            assert(l.compact && l.view_height >= 2 && l.view_width > 0);
            assert(l.view_y >= l.header_height && l.view_y + l.view_height < l.controls_y);
            if (l.title_y >= 0) assert(l.status_y - 7 > l.title_y);
            assert(l.status_y < l.button_y);
            solar_os_media_player_header_draw(&gfx, w, "WebRadio", "PLAYER [CHANNELS]");
            solar_os_media_player_controls_draw(&gfx, w, h, "long selected filename.wav",
                "PLAYING 12:34 / 99:59", 50, seek, SOLAR_OS_MEDIA_TRANSPORT_PAUSE);
            solar_os_media_player_status_draw(&gfx, w, h, "PAUSED 12:34");
            assert(last_rect_y >= l.controls_y && last_rect_y + last_rect_height <= l.button_y);
            for (int i = 0; i < l.button_count; i++) {
                int x = l.button_gap * (i + 1) + l.button_width * i;
                assert(solar_os_media_player_button_at(w, h, seek, x, l.button_y) == i);
                assert(solar_os_media_player_button_at(w, h, seek, x - 1, l.button_y) == -1);
                assert(solar_os_media_player_button_at(w, h, seek, x, l.button_y + l.button_height) == -1);
            }
            solar_os_media_dialog_draw(&gfx, w, h, "Recording filename", "very-long-filename-with-cursor_", "Enter save Esc cancel");
        }
        solar_os_media_list_layout_t list;
        solar_os_media_list_layout(w, h, &list);
        assert(list.visible > 0 && list.top >= 10);
        assert(list.top + list.visible * list.row_height <= h - list.footer_height);
    }
    check_bounds = false;
    /* The common glyphs occupy the same vertical span at common button sizes. */
    const solar_os_media_transport_icon_t icons[] = {
        SOLAR_OS_MEDIA_TRANSPORT_PREVIOUS, SOLAR_OS_MEDIA_TRANSPORT_REWIND,
        SOLAR_OS_MEDIA_TRANSPORT_PLAY, SOLAR_OS_MEDIA_TRANSPORT_PAUSE,
        SOLAR_OS_MEDIA_TRANSPORT_STOP, SOLAR_OS_MEDIA_TRANSPORT_FORWARD,
        SOLAR_OS_MEDIA_TRANSPORT_NEXT};
    const int heights[] = {21, 28, 40};
    for (size_t h = 0; h < sizeof(heights) / sizeof(*heights); h++) {
        int reference_top = 0, reference_bottom = 0;
        for (size_t i = 0; i < sizeof(icons) / sizeof(*icons); i++) {
            glyph_top = 1000; glyph_bottom = -1; polygons = 0;
            solar_os_media_transport_button_draw(&gfx, 5, 10, 42, heights[h], icons[i], false);
            if (i == 0) { reference_top = glyph_top; reference_bottom = glyph_bottom; }
            assert(glyph_top == reference_top && glyph_bottom == reference_bottom);
            if (icons[i] == SOLAR_OS_MEDIA_TRANSPORT_REWIND ||
                icons[i] == SOLAR_OS_MEDIA_TRANSPORT_FORWARD) assert(polygons == 2);
        }
    }
    webradio_worker_t worker = {.output_format.sample_rate = 16000};
    char status[96];
    int16_t sample = 0;
    webradio_progress_text(WEBRADIO_PLAYBACK_CONNECTING, "", status, sizeof(status));
    assert(strcmp(status, "CONNECTING 00:00") == 0);
    /* Converted output rate, not the MP3's source rate, determines play time. */
    webradio_player_samples(&sample, 32000, 2, &worker);
    assert(worker.playback_started);
    webradio_progress_text(WEBRADIO_PLAYBACK_PLAYING, "", status, sizeof(status));
    assert(strcmp(status, "PLAYING 00:01") == 0);
    webradio_progress_text(WEBRADIO_PLAYBACK_BUFFERING, "", status, sizeof(status));
    assert(strcmp(status, "BUFFERING 00:01") == 0);
    webradio_player_samples(&sample, 0, 0, &worker);
    webradio_player_samples(&sample, 32000, 2, &worker);
    webradio_progress_text(WEBRADIO_PLAYBACK_PLAYING, "", status, sizeof(status));
    assert(strcmp(status, "PLAYING 00:02") == 0);
    webradio.task = &worker;
    webradio_toggle_pause();
    assert(webradio.paused && webradio.redraw && webradio_should_pause(NULL));
    webradio_progress_text(WEBRADIO_PLAYBACK_BUFFERING, "", status, sizeof(status));
    assert(strcmp(status, "PAUSED 00:02") == 0);
    webradio.stop_requested = true;
    assert(!webradio_should_pause(NULL)); /* Stop must unblock producer and sink. */
    webradio.stop_requested = false;
    webradio_toggle_pause();
    assert(!webradio.paused && !webradio_should_pause(NULL));
    webradio.task_done = true;
    webradio_toggle_pause();
    assert(!webradio.paused);
    webradio_progress_text(WEBRADIO_PLAYBACK_ERROR, "network unavailable", status, sizeof(status));
    assert(strcmp(status, "ERROR - network unavailable") == 0);
    assert(draws > 0);
    puts("media transport UI tests passed");
    return 0;
}
