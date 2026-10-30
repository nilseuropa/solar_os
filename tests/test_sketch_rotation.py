from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class SketchRotationTest(unittest.TestCase):
    def test_document_and_pointer_mapping_survive_viewport_changes(self):
        sketch = (ROOT / 'src/apps/solar_os_sketch.c').read_text()
        layout = re.search(r'typedef struct \{\n    int screen_width;.*?} sketch_layout_t;', sketch, re.S).group()
        source = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "solar_os_sketch_canvas.h"
#define SOLAR_OS_INPUT_POINTER_PRESS 1
#define SOLAR_OS_INPUT_POINTER_MOVE 2
#define SOLAR_OS_INPUT_POINTER_RELEASE 3
typedef struct {int action;} solar_os_input_pointer_event_t;
static struct {solar_os_sketch_canvas_t canvas;int pointer_x,pointer_y,tool;
    int stroke_x,stroke_y,preview_x,preview_y;bool drawing,dirty,render_pending;
    unsigned weight;} sketch;
#define SKETCH_TOOL_FILL 4
static bool sketch_is_shape_tool(void) {return false;}
static bool sketch_is_brush_tool(void) {return true;}
static uint8_t sketch_brush_color(void) {return 3;}
static uint8_t sketch_brush_pattern(void) {return 0;}
static void sketch_bucket_fill(int x,int y) {(void)x;(void)y;assert(false);}
static void sketch_draw_shape(solar_os_sketch_canvas_t *c,int x,int y) {(void)c;(void)x;(void)y;assert(false);}
'''
        source += layout + production_functions(sketch, ('sketch_canvas_viewport', 'sketch_canvas_pointer'))
        source += r'''
int main(void) {
    uint8_t pixels[(336*176+3)/4], original[sizeof(pixels)];
    sketch.canvas=(solar_os_sketch_canvas_t){pixels,336,176};sketch.weight=1;
    solar_os_sketch_canvas_clear(&sketch.canvas,0);
    solar_os_sketch_canvas_line(&sketch.canvas,0,0,335,175,3,0,1);
    memcpy(original,pixels,sizeof(pixels));
    for(unsigned rotation=0;rotation<8;rotation++) {
        sketch_layout_t layout={.canvas_x=64,.canvas_y=24,
            .canvas_width=rotation&1?176:336,.canvas_height=rotation&1?336:176};
        sketch_layout_t view=sketch_canvas_viewport(&layout);
        assert(view.canvas_width<=layout.canvas_width && view.canvas_height<=layout.canvas_height);
        assert(view.canvas_width<=336 && view.canvas_height<=176);
        assert(view.canvas_x>=layout.canvas_x && view.canvas_y>=layout.canvas_y);
        assert(!memcmp(original,pixels,sizeof(pixels)));
    }
    sketch_layout_t portrait={.canvas_x=64,.canvas_y=44,.canvas_width=176,.canvas_height=336};
    sketch_layout_t view=sketch_canvas_viewport(&portrait);
    sketch.pointer_x=view.canvas_x+view.canvas_width/2;
    sketch.pointer_y=view.canvas_y+view.canvas_height/2;
    solar_os_input_pointer_event_t event={SOLAR_OS_INPUT_POINTER_PRESS};
    sketch_canvas_pointer(&portrait,&event);
    assert(sketch.dirty && sketch.drawing);
    assert(sketch.stroke_x==168 && sketch.stroke_y==88);
    assert(solar_os_sketch_canvas_get(&sketch.canvas,168,88)==3);
    event.action=SOLAR_OS_INPUT_POINTER_RELEASE;sketch_canvas_pointer(&portrait,&event);
    assert(!sketch.drawing);
    sketch.dirty=false;sketch.pointer_y=view.canvas_y-1;
    event.action=SOLAR_OS_INPUT_POINTER_PRESS;sketch_canvas_pointer(&portrait,&event);
    assert(!sketch.drawing && !sketch.dirty);
    return 0;
}
'''
        self.run_native(source, [ROOT / 'src/apps/solar_os_sketch_canvas.c'])

    def test_scaled_packed_color_pixels_and_clipping(self):
        gfx = (ROOT / 'src/services/solar_os_gfx.c').read_text()
        source = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef unsigned solar_os_gfx_color_t;
typedef unsigned u8g2_uint_t;
static struct {uint8_t pixels[24];} mono;
typedef struct {struct {unsigned stride;} surface;uint8_t pixels[24];} indexed_t;
typedef struct {void *u8g2;indexed_t *index8;} solar_os_gfx_t;
static bool gfx_ready(solar_os_gfx_t *g) {return g!=NULL;}
static bool gfx_uses_index8(solar_os_gfx_t *g) {return g->index8!=NULL;}
static unsigned gfx_index8_for_color(unsigned color) {return color;}
static unsigned solar_os_gfx_width(solar_os_gfx_t *g) {(void)g;return 6;}
static unsigned solar_os_gfx_height(solar_os_gfx_t *g) {(void)g;return 4;}
static unsigned gfx_pattern_draw_color(solar_os_gfx_t *g,unsigned c,int x,int y) {(void)g;(void)x;(void)y;return c;}
static void u8g2_SetDrawColor(void *g,unsigned c) {(void)g;mono.pixels[23]=c;}
static void u8g2_DrawPixel(void *g,unsigned x,unsigned y) {(void)g;assert(x<6&&y<4);mono.pixels[y*6+x]=mono.pixels[23];}
static void gfx_mark_index8_dirty_rect(solar_os_gfx_t *g,int x,int y,int w,int h) {(void)g;(void)x;(void)y;(void)w;(void)h;}
static void gfx_mark_dirty(solar_os_gfx_t *g) {(void)g;}
'''
        source += production_functions(gfx, ('solar_os_gfx_bitmap_2bpp_scaled', 'solar_os_gfx_bitmap_2bpp'))
        source += r'''
int main(void) {
    const uint8_t packed[]={0xe4,0x93,0x4e};
    const unsigned palette[]={0,1,2,3};
    indexed_t indexed={.surface={6}};
    for(unsigned use_index=0;use_index<2;use_index++) {
        solar_os_gfx_t g={.index8=use_index?&indexed:NULL};
        memset(indexed.pixels,9,24);memset(mono.pixels,9,24);
        solar_os_gfx_bitmap_2bpp_scaled(&g,-1,0,5,3,packed,sizeof(packed),4,3,palette);
        uint8_t *out=use_index?indexed.pixels:mono.pixels;
        for(unsigned y=0;y<3;y++) for(unsigned x=0;x<4;x++) {
            unsigned pixel=y*4+(x+1)*4/5;
            assert(out[y*6+x]==((packed[pixel/4]>>((pixel%4)*2))&3));
        }
        assert(out[4]==9 && out[5]==9);
        solar_os_gfx_bitmap_2bpp(&g,0,0,4,3,packed,sizeof(packed),palette);
        for(unsigned y=0;y<3;y++) for(unsigned x=0;x<4;x++)
            assert(out[y*6+x]==((packed[y]>>(x*2))&3));
    }
    return 0;
}
'''
        self.run_native(source, [])

    def run_native(self, source, extra):
        with tempfile.TemporaryDirectory() as directory:
            path, binary = Path(directory, 'rotation.c'), Path(directory, 'rotation')
            path.write_text(source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'src/apps'), str(path), *map(str, extra), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
