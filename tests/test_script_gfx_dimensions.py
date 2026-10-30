from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class ScriptGfxDimensionsTest(unittest.TestCase):
    def test_native_presentation_does_not_change_logical_size(self):
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 20
typedef struct {unsigned width,height;} u8g2_t;
typedef struct {u8g2_t *u8g2;} solar_os_gfx_t;
typedef struct {uint16_t width,height;} solar_os_display_target_t;
static bool registered=true;
static solar_os_display_target_t target={400,240};
static bool gfx_ready(const solar_os_gfx_t *g) {return g && g->u8g2;}
static bool solar_os_gfx_display_target_name(const solar_os_gfx_t *g,char *out,size_t n) {
    (void)g;(void)out;(void)n;return registered;
}
static bool solar_os_display_find_target(const char *n,solar_os_display_target_t *out) {
    (void)n;*out=target;return true;
}
static unsigned u8g2_GetDisplayWidth(u8g2_t *u) {return u->width;}
static unsigned u8g2_GetDisplayHeight(u8g2_t *u) {return u->height;}
'''
        source += production_functions((ROOT / 'src/services/solar_os_gfx.c').read_text(),
                                       ('solar_os_gfx_dimensions',))
        source += r'''
int main(void) {
    u8g2_t native={800,480};solar_os_gfx_t gfx={&native};uint16_t w,h;
    assert(solar_os_gfx_dimensions(&gfx,&w,&h) && w==400 && h==240);
    target=(solar_os_display_target_t){240,400};
    assert(solar_os_gfx_dimensions(&gfx,&w,&h) && w==240 && h==400);
    native=(u8g2_t){480,800};
    assert(solar_os_gfx_dimensions(&gfx,&w,&h) && w==240 && h==400);
    registered=false;
    assert(solar_os_gfx_dimensions(&gfx,&w,&h) && w==480 && h==800);
    assert(!solar_os_gfx_dimensions(NULL,&w,&h) && w==0 && h==0);
    assert(!solar_os_gfx_dimensions(&gfx,NULL,&h));return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path, binary = Path(directory, 'dimensions.c'), Path(directory, 'dimensions')
            path.write_text(source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
