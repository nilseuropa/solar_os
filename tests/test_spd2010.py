from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Spd2010RasterTest(unittest.TestCase):
    def test_rgb_indexed_scaling_rotation_and_bounds(self):
        source = r'''
#include <assert.h>
#include <string.h>
#include "solar_os_rgb_panel_buffer.h"
static uint16_t color(const solar_os_rgb_panel_buffer_t *b, unsigned x, unsigned y) {
    const uint8_t *p = b->pixels + (y * b->width + x) * 2;
    return ((uint16_t)p[0] << 8) | p[1];
}
int main(void) {
    uint8_t storage[4 + 3*2*2 + 4]; memset(storage, 0xa5, sizeof(storage));
    solar_os_rgb_panel_buffer_t b = {.pixels=storage+4, .width=3, .height=2};
    const uint8_t pixels[] = {0x12,0x34, 0x56,0x78, 0x9a,0xbc, 0xde,0xf0, 0x11,0x22, 0x33,0x44};
    solar_os_display_raster_t f = {.data=pixels, .data_size=sizeof(pixels),
        .source_width=3, .source_height=2, .source_stride=6,
        .width=3, .height=2, .format=SOLAR_OS_DISPLAY_FORMAT_RGB565};
    assert(solar_os_rgb_panel_frame(&b,&f,SOLAR_OS_DISPLAY_ROTATION_0)==ESP_OK);
    assert(!memcmp(b.pixels,pixels,sizeof(pixels)));
    assert(solar_os_rgb_panel_frame(&b,&f,SOLAR_OS_DISPLAY_ROTATION_180)==ESP_OK);
    assert(color(&b,0,0)==0x3344 && color(&b,2,1)==0x1234);
    /* Quarter turns accept logical dimensions swapped against native storage. */
    f.source_width=f.width=2; f.source_height=f.height=3; f.source_stride=4;
    assert(solar_os_rgb_panel_frame(&b,&f,SOLAR_OS_DISPLAY_ROTATION_90)==ESP_OK);
    assert(color(&b,0,0)==0x1122 && color(&b,2,0)==0x1234 && color(&b,0,1)==0x3344);
    assert(solar_os_rgb_panel_frame(&b,&f,SOLAR_OS_DISPLAY_ROTATION_270)==ESP_OK);
    assert(color(&b,0,0)==0x5678 && color(&b,2,0)==0x3344 && color(&b,0,1)==0x1234);
    /* Scale a one-pixel overlay; preserve other pixels unless explicitly cleared. */
    solar_os_rgb_panel_clear(&b,0xabcd);
    f=(solar_os_display_raster_t){.data=pixels,.data_size=2,.source_width=1,.source_height=1,
        .source_stride=2,.x=1,.width=2,.height=2,.format=SOLAR_OS_DISPLAY_FORMAT_RGB565};
    assert(solar_os_rgb_panel_frame(&b,&f,0)==ESP_OK);
    assert(color(&b,0,0)==0xabcd && color(&b,1,0)==0x1234 && color(&b,2,1)==0x1234);
    f.width=f.height=1; f.clear_background=true; f.background_rgb565=0xffff;
    assert(solar_os_rgb_panel_frame(&b,&f,0)==ESP_OK);
    assert(color(&b,0,0)==0xffff && color(&b,1,0)==0x1234 && color(&b,2,1)==0xffff);
    uint8_t before[12]; memcpy(before,b.pixels,12);
    f.data_size=1;
    assert(solar_os_rgb_panel_frame(&b,&f,0)==ESP_ERR_INVALID_ARG && !memcmp(before,b.pixels,12));
    f.data_size=2; f.x=3;
    assert(solar_os_rgb_panel_frame(&b,&f,0)==ESP_ERR_INVALID_SIZE && !memcmp(before,b.pixels,12));
    f.x=0; f.palette_inverted=true;
    assert(solar_os_rgb_panel_frame(&b,&f,0)==ESP_ERR_INVALID_ARG);
    uint16_t palette[256]; for (unsigned i=0;i<256;i++) palette[i]=(uint16_t)(i*257);
    const uint8_t indices[]={0xe4,0x00}; /* INDEX2 pixels 0,1,2,3 packed low first. */
    f=(solar_os_display_raster_t){.data=indices,.data_size=2,.source_width=4,.source_height=1,
        .source_stride=1,.width=3,.height=1,.format=SOLAR_OS_DISPLAY_FORMAT_INDEX2,
        .palette_rgb565=palette,.palette_size=4};
    assert(solar_os_rgb_panel_frame(&b,&f,0)==ESP_OK);
    assert(color(&b,0,0)==palette[0] && color(&b,1,0)==palette[1] && color(&b,2,0)==palette[2]);
    f.palette_inverted=true;
    assert(solar_os_rgb_panel_frame(&b,&f,0)==ESP_OK && color(&b,0,0)==palette[3]);
    f.clear_background=true; f.background_index=4;
    assert(solar_os_rgb_panel_frame(&b,&f,0)==ESP_ERR_INVALID_ARG);
    const uint8_t surface_data[]={1,2,3,4,5,6};
    solar_os_display_surface_t s={.data=surface_data,.data_size=6,.width=2,.height=3,
        .stride=2,.native_width=3,.native_height=2,.format=SOLAR_OS_DISPLAY_FORMAT_INDEX8,
        .palette_rgb565=palette,.palette_size=256,.rotation=SOLAR_OS_DISPLAY_ROTATION_90};
    assert(solar_os_rgb_panel_surface(&b,&s)==ESP_OK);
    assert(color(&b,0,0)==palette[5] && color(&b,2,1)==palette[2]);
    s.data_size=5;
    assert(solar_os_rgb_panel_surface(&b,&s)==ESP_ERR_INVALID_ARG);
    s.data_size=6; s.native_width=2;
    assert(solar_os_rgb_panel_surface(&b,&s)==ESP_ERR_INVALID_SIZE);
    for (unsigned i=0;i<4;i++) assert(storage[i]==0xa5 && storage[16+i]==0xa5);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(source)
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/drivers"),
                "-I", str(ROOT / "src/services"),
                str(path / "test.c"), str(ROOT / "src/drivers/solar_os_rgb_panel_buffer.c"),
                "-o", str(path / "test"),
            ], check=True)
            subprocess.run([str(path / "test")], check=True)
