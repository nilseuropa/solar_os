from pathlib import Path
import unittest
from test_foreground_presenter import production_functions
from test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class ViewColorTest(unittest.TestCase):
    def test_photo_colors_clipping_animation_resume_and_fallback(self):
        source = (ROOT / "src/apps/solar_os_view.c").read_text()
        state = source[source.index("typedef enum {"):source.index("static uint16_t view_read_le16")]
        functions = production_functions(source, (
            "view_free_image", "view_current_frame_pixels", "view_clamp_pan",
            "view_fit_dimensions", "view_draw_scaled", "view_present_color",
            "view_render", "view_stop", "view_resume"))
        run_c(r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os.h"
#include "solar_os_gfx.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
struct solar_os_gfx { solar_os_display_rect_t panel, canvas; };
static bool color = true, fail_alloc;
static esp_err_t present_error;
static unsigned direct, indexed, backgrounds, allocations;
static int x, y, w, h;
static uint8_t output[512];
void *solar_os_memory_realloc(void *ptr, size_t size, solar_os_memory_class_t kind, const char *tag) {
    assert(kind == SOLAR_OS_MEMORY_EXTERNAL_REQUIRED && !strcmp(tag,"view.rgb565"));
    if (fail_alloc) return NULL;
    allocations++; return realloc(ptr,size);
}
void solar_os_memory_free(void *ptr) { free(ptr); }
solar_os_gfx_t *solar_os_context_gfx(solar_os_context_t *ctx) { return ctx->gfx; }
void solar_os_context_set_graphics_active(solar_os_context_t *ctx,bool active) { (void)ctx;(void)active; }
solar_os_display_rect_t solar_os_gfx_canvas_bounds(const solar_os_gfx_t *gfx) { return gfx->canvas; }
solar_os_display_rect_t solar_os_gfx_panel_bounds(const solar_os_gfx_t *gfx) { return gfx->panel; }
void solar_os_gfx_clear(solar_os_gfx_t *gfx,solar_os_gfx_color_t value) { (void)gfx;(void)value; }
void solar_os_gfx_present(solar_os_gfx_t *gfx) { (void)gfx;backgrounds++; }
bool solar_os_gfx_supports_frame_format(const solar_os_gfx_t *gfx,solar_os_display_format_t format)
{ (void)gfx;assert(format == SOLAR_OS_DISPLAY_FORMAT_RGB565);return color; }
esp_err_t solar_os_gfx_present_frame_at(solar_os_gfx_t *gfx,const solar_os_display_raster_t *r,int left,int top) {
    (void)gfx;assert(r->format == SOLAR_OS_DISPLAY_FORMAT_RGB565 && !r->palette_inverted);
    assert(r->width == r->source_width && r->height == r->source_height);
    assert(r->source_stride == r->width*2 && r->data_size == (size_t)r->width*r->height*2);
    assert(r->data_size <= sizeof(output));memcpy(output,r->data,r->data_size);
    direct++;x=left;y=top;w=r->width;h=r->height;return present_error;
}
esp_err_t solar_os_gfx_blit_raster(solar_os_gfx_t *gfx,const solar_os_gfx_raster_t *r,
    int left,int top,int width,int height,const solar_os_gfx_clip_t *clip) {
    (void)gfx;(void)r;(void)left;(void)top;(void)width;(void)height;(void)clip;indexed++;return ESP_OK;
}
''' + state + functions + r'''
int main(void) {
    solar_os_gfx_t gfx={.canvas={0,0,4,4},.panel={-1,-1,6,6}};
    solar_os_context_t ctx={.gfx=&gfx};view_state_t state={0};view_state_storage=&state;
    state.loaded=true;
    state.image=(view_image_t){.width=2,.height=2,.channels=3,.frame_count=2};
    const uint8_t rgb[]={24,28,32, 72,68,80, 255,0,0, 0,255,0,
                         0,0,255, 255,255,255, 0,0,0, 255,255,0};
    state.image.pixels=malloc(sizeof(rgb));assert(state.image.pixels);memcpy(state.image.pixels,rgb,sizeof(rgb));
    view_render(&ctx);
    assert(direct==1 && !indexed && backgrounds==1 && x==0 && y==0 && w==4 && h==4);
    /* Preserve near-neutral colors that the indexed palette collapses/changes. */
    const uint8_t first[]={0x18,0xe4,0x18,0xe4,0x4a,0x2a,0x4a,0x2a};
    assert(!memcmp(output,first,sizeof(first)));
    assert(output[16]==0xf8 && output[17]==0 && output[22]==0x07 && output[23]==0xe0);
    state.image.frame_index=1;view_render(&ctx);
    assert(direct==2 && backgrounds==1 && allocations==1 && output[0]==0 && output[1]==0x1f);
    view_resume(&ctx);assert(backgrounds==2 && direct==3);
    /* Actual-size pan clips to the physical panel, even beyond the canvas. */
    view_state.direct_presented=false;
    assert(view_present_color(&gfx,-2,-2,8,8));
    assert(x==-1 && y==-1 && w==6 && h==6 && output[0]==0 && output[1]==0x1f);
    assert(output[10]==0xff && output[11]==0xff && output[60]==0 && output[61]==0);
    color=false;view_render(&ctx);assert(indexed==1);
    color=true;present_error=ESP_FAIL;view_render(&ctx);assert(indexed==2 && !state.direct_presented);
    present_error=ESP_OK;
    free(state.rgb565);state.rgb565=NULL;state.rgb565_capacity=0;
    fail_alloc=true;view_render(&ctx);assert(indexed==3 && !state.rgb565);
    fail_alloc=false;view_render(&ctx);assert(state.rgb565 && state.direct_presented);
    view_stop(&ctx);assert(!state.rgb565 && !state.image.pixels);
}
''', "-I", str(ROOT / "src/services"), "-I", str(ROOT / "src"),
              "-idirafter", str(ROOT / "include"), "-fsanitize=address,undefined", "-no-pie")
