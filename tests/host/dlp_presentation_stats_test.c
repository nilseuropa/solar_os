#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dlp.h"
#include "solar_os_dlp_receiver.h"
#include "solar_os_dlp_raster.h"
#include "solar_os_display_surface.h"
#include "dlp_presentation_stats.inc"

typedef int esp_err_t;
#define ESP_OK 0
#define SOLAR_OS_GFX_COLOR_WHITE 0
#define SOLAR_OS_GFX_COLOR_BLACK 3

typedef struct {
    unsigned width, height;
    esp_err_t error;
    unsigned calls;
    unsigned error_on_call;
    solar_os_display_raster_t last;
    uint8_t panel[512];
} fake_gfx_t;
typedef struct {
    fake_gfx_t *gfx;
    bool suspended, direct_mono, direct_index2, raster_valid;
    uint8_t *indexed;
    unsigned presented_format;
    size_t presented_size;
    solar_os_dlp_receiver_t receiver;
    solar_os_dlp_presentation_stats_t presentation;
} dlp_app_state_t;

static int64_t clock_us = 100;
static int64_t esp_timer_get_time(void) { return clock_us; }
static size_t solar_os_gfx_width(fake_gfx_t *gfx) { return gfx->width; }
static size_t solar_os_gfx_height(fake_gfx_t *gfx) { return gfx->height; }
static void solar_os_gfx_clear(fake_gfx_t *gfx, int color)
{ (void)gfx; (void)color; clock_us += 50; }
static void solar_os_gfx_set_color(fake_gfx_t *gfx, int color)
{ (void)gfx; (void)color; clock_us += 10; }
static void solar_os_gfx_bitmap(fake_gfx_t *gfx, int x, int y, int w, int h, const uint8_t *data)
{ (void)gfx; (void)x; (void)y; (void)w; (void)h; (void)data; clock_us += 150; }
static void solar_os_gfx_present(fake_gfx_t *gfx)
{ (void)gfx; clock_us += 300; }
static esp_err_t solar_os_gfx_present_frame(fake_gfx_t *gfx, const solar_os_display_raster_t *frame)
{
    assert(frame->x + frame->width <= gfx->width);
    assert(frame->y + frame->height <= gfx->height);
    gfx->last = *frame;
    gfx->calls++;
    if (frame->format == SOLAR_OS_DISPLAY_FORMAT_INDEX2) {
        assert(frame->data_size == (size_t)frame->source_stride * frame->height);
        assert(frame->palette_size == 4 && frame->palette_rgb565[0] == 0xffff &&
            frame->palette_rgb565[3] == 0);
        for (unsigned y = 0; y < frame->height; y++)
            for (unsigned x = 0; x < frame->width; x++)
                gfx->panel[(frame->y + y) * gfx->width + frame->x + x] =
                    (frame->data[y * frame->source_stride + x / 4U] >> (2U * (x % 4U))) & 3U;
    }
    clock_us += 70;
    return gfx->calls == gfx->error_on_call ? -1 : gfx->error;
}

#include "dlp_presentation_functions.inc"

int main(void)
{
    fake_gfx_t gfx = {.width = 16, .height = 8};
    dlp_app_state_t app = {.gfx = &gfx};
    uint8_t image[256] = {0};
    assert(present_image(&app, DLP_MONO1, image, 16, 16, 8) == DLP_OK);
    assert(app.presentation.frames == 1);
    assert(app.presented_format == DLP_MONO1 && app.presented_size == 16);
    assert(app.presentation.render_last_us == 210);
    assert(app.presentation.display_last_us == 300);
    assert(app.presentation.total_last_us == 510);
    assert(app.presentation.first_completed_us == 610);
    assert(present_image(&app, DLP_RGB565_BE, image, 256, 16, 8) == DLP_OK);
    assert(app.presentation.frames == 2);
    assert(app.presented_format == DLP_RGB565_BE && app.presented_size == 256);
    assert(app.presentation.render_last_us == 0);
    assert(app.presentation.display_last_us == 70);
    assert(app.presentation.render_max_us == 210 && app.presentation.display_max_us == 300);
    assert(app.presentation.total_max_us == 510 && app.presentation.total_us == 580);
    assert(app.presentation.render_total_us == 210 && app.presentation.display_total_us == 370);
    assert(app.presentation.last_completed_us == 680);
    uint32_t render = 0, display = 0;
    assert(draw_image(&app, DLP_MONO1, image, 16, 16, 8, &render, &display) == DLP_OK);
    assert(render == 210 && display == 300 && app.presentation.frames == 2);
    app.suspended = true;
    assert(present_image(&app, DLP_MONO1, image, 16, 16, 8) == DLP_BUSY);
    assert(app.presentation.frames == 2);
    app.suspended = false;
    gfx.error = -1;
    assert(present_image(&app, DLP_RGB565_BE, image, 256, 16, 8) == DLP_IO_ERROR);
    assert(app.presentation.frames == 2 && app.presentation.total_us == 580);
    assert(app.presented_format == DLP_RGB565_BE && app.presented_size == 256);
    assert(present_image(&app, DLP_MONO1, image, 16, 15, 8) == DLP_IO_ERROR);
    gfx.error = 0;
    gfx.width = 13;
    uint8_t previous[16] = {0}, next[16] = {0}, indexed[32];
    app.direct_index2 = true;
    app.indexed = indexed;
    app.receiver.front = previous;
    app.receiver.valid = true;
    assert(present_image(&app, DLP_MONO1, next, 16, 13, 8) == DLP_OK);
    assert(gfx.last.x == 0 && gfx.last.width == 13 && gfx.last.height == 8);
    assert(app.raster_valid);
    unsigned calls = gfx.calls;
    assert(present_image(&app, DLP_MONO1, next, 16, 13, 8) == DLP_OK);
    assert(gfx.calls == calls && app.presentation.display_last_us == 0);
    next[7] = 0x10; /* x=12, y=3 */
    assert(present_image(&app, DLP_MONO1, next, 16, 13, 8) == DLP_OK);
    assert(gfx.last.x == 8 && gfx.last.y == 3 && gfx.last.width == 5 && gfx.last.height == 1);
    assert(gfx.panel[3 * gfx.width + 12] == 3);
    memcpy(previous, next, sizeof(next));
    /* Overlay redraw uses front and repaints the full panel without counting. */
    uint32_t frames = app.presentation.frames;
    memset(gfx.panel, 0xa5, sizeof(gfx.panel));
    assert(draw_image(&app, DLP_MONO1, previous, 16, 13, 8, &render, &display) == DLP_OK);
    assert(gfx.last.width == 13 && gfx.last.height == 8 && app.presentation.frames == frames);
    for (unsigned i = 0; i < 13 * 8; i++) assert(gfx.panel[i] == (i == 3 * 13 + 12 ? 3 : 0));
    /* A partially failed write cannot leave a valid comparison baseline. */
    next[0] = 1;
    gfx.error = -1;
    assert(present_image(&app, DLP_MONO1, next, 16, 13, 8) == DLP_IO_ERROR);
    assert(!app.raster_valid && app.presentation.frames == frames);
    gfx.error = 0;
    assert(present_image(&app, DLP_MONO1, next, 16, 13, 8) == DLP_OK);
    assert(gfx.last.width == 13 && gfx.last.height == 8);
    /* Changing presentation format also requires a full MONO repaint. */
    app.receiver.valid = true;
    assert(present_image(&app, DLP_RGB565_BE, image, 208, 13, 8) == DLP_OK);
    assert(!app.raster_valid);
    assert(present_image(&app, DLP_MONO1, next, 16, 13, 8) == DLP_OK);
    assert(gfx.last.width == 13 && gfx.last.height == 8);
    /* Reconfigure invalidates the receiver front, even with a valid panel. */
    app.receiver.valid = false;
    assert(present_image(&app, DLP_MONO1, next, 16, 13, 8) == DLP_OK);
    assert(gfx.last.width == 13 && gfx.last.height == 8);
    /* Complete a multi-region frame only after every region succeeds. */
    gfx.height = 20;
    uint8_t old_tall[40] = {0}, new_tall[40] = {0}, indexed_tall[80];
    new_tall[1] = new_tall[38] = 1;
    app.indexed = indexed_tall;
    app.receiver.front = old_tall;
    app.receiver.valid = app.raster_valid = true;
    gfx.error_on_call = gfx.calls + 2;
    frames = app.presentation.frames;
    assert(present_image(&app, DLP_MONO1, new_tall, 40, 13, 20) == DLP_IO_ERROR);
    assert(gfx.calls == gfx.error_on_call && !app.raster_valid && app.presentation.frames == frames);
    assert(present_image(&app, DLP_MONO1, new_tall, 40, 13, 20) == DLP_OK);
    assert(gfx.last.width == 13 && gfx.last.height == 20 && app.presentation.frames == frames + 1);
    puts("DLP timing phases, maxima, direct raster, redraw exclusion and failed frames PASS");
    return 0;
}
