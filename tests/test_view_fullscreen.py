from pathlib import Path
import unittest
from test_foreground_presenter import production_functions
from test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class ViewFullscreenTest(unittest.TestCase):
    def test_fullscreen_fit_actual_pan_and_return_to_canvas(self):
        source = (ROOT / "src/apps/solar_os_view.c").read_text()
        state = source[source.index("typedef enum {"):source.index("static uint16_t view_read_le16")]
        functions = production_functions(source, (
            "view_current_frame_pixels", "view_reset_pan", "view_clamp_pan",
            "view_fit_dimensions", "view_draw_scaled", "view_render", "view_event"))
        run_c(r'''
#include <assert.h>
#include <string.h>
#include "solar_os.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_storage.h"
#define VIEW_PAN_STEP 32
struct solar_os_gfx { solar_os_display_rect_t panel, canvas; };
struct solar_os_context { solar_os_gfx_t *gfx; };
static int x,y,w,h;
solar_os_gfx_t *solar_os_context_gfx(solar_os_context_t *ctx) { return ctx->gfx; }
solar_os_display_rect_t solar_os_gfx_canvas_bounds(const solar_os_gfx_t *gfx) { return gfx->canvas; }
solar_os_display_rect_t solar_os_gfx_panel_bounds(const solar_os_gfx_t *gfx) { return gfx->panel; }
void solar_os_gfx_clear(solar_os_gfx_t *gfx, solar_os_gfx_color_t color) { (void)gfx;(void)color; }
void solar_os_gfx_present(solar_os_gfx_t *gfx) { (void)gfx; }
esp_err_t solar_os_gfx_blit_raster(solar_os_gfx_t *gfx, const solar_os_gfx_raster_t *raster,
    int left,int top,int width,int height,const solar_os_gfx_clip_t *clip) {
    (void)gfx;(void)raster;(void)clip;x=left;y=top;w=width;h=height;return ESP_OK;
}
void solar_os_context_finish(solar_os_context_t *ctx,int status,const char *message)
{ (void)ctx;(void)status;(void)message; }
static void view_resume(solar_os_context_t *ctx) { (void)ctx; }
static void view_advance_animation(solar_os_context_t *ctx,uint32_t now) { (void)ctx;(void)now; }
''' + state + functions + r'''
int main(void) {
    solar_os_gfx_t gfx={.panel={-60,-60,412,412},.canvas={0,0,291,291}};
    solar_os_context_t ctx={.gfx=&gfx};
    view_state_t state={0};view_state_storage=&state;
    uint8_t pixels=0;state.loaded=true;
    state.image=(view_image_t){.pixels=&pixels,.channels=1,.width=640,.height=360};
    view_render(&ctx);assert(x==0 && y==64 && w==291 && h==163);
    solar_os_event_t key={.type=SOLAR_OS_EVENT_CHAR,.data.ch='f'};
    assert(view_event(&ctx,&key) && state.fullscreen && state.mode==VIEW_MODE_FIT);
    assert(x==-60 && y==30 && w==412 && h==231);
    key.data.ch='0';assert(view_event(&ctx,&key));
    assert(state.mode==VIEW_MODE_ACTUAL && state.fullscreen && w==640 && h==360);
    assert(x==-174 && y==-34);
    state.pan_x=-9999;state.pan_y=9999;view_render(&ctx);
    assert(x==-288 && y==-34); /* Pan clamps against panel, not canvas. */
    key.data.ch='f';assert(view_event(&ctx,&key) && !state.fullscreen);
    assert(x==-174 && y==-34 && w==640 && h==360);
    key.data.ch='1';assert(view_event(&ctx,&key));
    assert(x==0 && y==64 && w==291 && h==163);
    gfx.panel=gfx.canvas;key.data.ch='f';assert(view_event(&ctx,&key));
    assert(x==0 && y==64 && w==291 && h==163);
}
''', "-I", str(ROOT / "src/services"), "-I", str(ROOT / "src"),
              "-idirafter", str(ROOT / "include"), "-fsanitize=address,undefined", "-no-pie")
