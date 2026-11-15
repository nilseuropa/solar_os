from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions
from test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class Spd2010RasterTest(unittest.TestCase):
    def test_shutdown_preserves_shared_wake_supply_and_retries_failures(self):
        source = production_functions((ROOT / "src/drivers/spd2010.c").read_text(),
                                      ("solar_os_spd2010_shutdown",))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include "esp_err.h"
typedef struct {
    struct { bool have_power, power_active_high, keep_power_on_shutdown; int power; } config;
    bool ready;
} solar_os_spd2010_t;
static unsigned sleeps, cutoffs;
static esp_err_t sleep_error, power_error;
static esp_err_t solar_os_spd2010_power_save(solar_os_spd2010_t *d, bool enabled) {
    assert(d && d->ready && enabled); sleeps++; return sleep_error;
}
static esp_err_t control(const int *line, bool level) {
    assert(*line == 9 && !level && sleeps > 0); cutoffs++; return power_error;
}
''' + source + r'''
int main(void) {
    solar_os_spd2010_t d = {.ready = true,
        .config = {.have_power = true, .power_active_high = true, .power = 9}};
    assert(solar_os_spd2010_shutdown(&d) == ESP_OK && !d.ready && cutoffs == 1);
    d.ready = true; d.config.keep_power_on_shutdown = true;
    assert(solar_os_spd2010_shutdown(&d) == ESP_OK && !d.ready && cutoffs == 1);
    d.ready = true; d.config.keep_power_on_shutdown = false; sleep_error = ESP_FAIL;
    assert(solar_os_spd2010_shutdown(&d) == ESP_FAIL && d.ready && cutoffs == 1);
    sleep_error = ESP_OK; power_error = ESP_FAIL;
    assert(solar_os_spd2010_shutdown(&d) == ESP_FAIL && d.ready && cutoffs == 2);
    power_error = ESP_OK;
    assert(solar_os_spd2010_shutdown(&d) == ESP_OK && !d.ready && cutoffs == 3);
}
''')

    def test_unscaled_rgb_rows_preserve_padding_and_track_only_changed_rows(self):
        run_c(r'''
#include <assert.h>
#include <string.h>
#include "solar_os_rgb_panel_buffer.h"
int main(void) {
    uint8_t storage[16+10*6*2+16]; memset(storage,0xa5,sizeof(storage));
    uint8_t dirty=0;
    solar_os_rgb_panel_buffer_t b={.pixels=storage+16,.width=10,.height=6,.dirty_rows=&dirty};
    solar_os_rgb_panel_clear(&b,0xffff); dirty=0;
    uint8_t source[16]={0x12,0x34,0xab,0xcd,0x45,0x67,0xa5,0xa5,
                        0x89,0xab,0x23,0x45,0x67,0x89,0xa5,0xa5};
    solar_os_display_raster_t f={.data=source,.data_size=sizeof(source),
        .source_width=3,.source_height=2,.source_stride=8,.x=2,.y=2,.width=3,.height=2,
        .format=SOLAR_OS_DISPLAY_FORMAT_RGB565};
    assert(solar_os_rgb_panel_frame(&b,&f,0)==ESP_OK && dirty==0x0c);
    for (unsigned y=0;y<6;y++) for (unsigned x=0;x<10;x++) {
        const uint8_t *expected=y>=2 && y<4 && x>=2 && x<5 ? source+(y-2)*8+(x-2)*2 : NULL;
        const uint8_t *p=b.pixels+(y*10+x)*2;
        assert(p[0]==(expected ? expected[0] : 0xff) && p[1]==(expected ? expected[1] : 0xff));
    }
    dirty=0;
    assert(solar_os_rgb_panel_frame(&b,&f,0)==ESP_OK && !dirty);
    source[8]=0x90;
    assert(solar_os_rgb_panel_frame(&b,&f,0)==ESP_OK && dirty==0x08);
    for (unsigned i=0;i<16;i++) assert(storage[i]==0xa5 && storage[16+10*6*2+i]==0xa5);
}
''', "-I", str(ROOT / "src/drivers"), "-I", str(ROOT / "src/services"),
              str(ROOT / "src/drivers/solar_os_rgb_panel_buffer.c"),
              "-fsanitize=address,undefined", "-no-pie")

    def test_monochrome_white_bit_matches_shared_palette_contract(self):
        source = production_functions((ROOT / "src/drivers/spd2010.c").read_text(),
                                      ("from_u8x8", "hash_bytes", "display_callback"))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "u8g2.h"
#include "solar_os_rgb_panel_buffer.h"
#define WIDTH 412U
#define HEIGHT 412U
#define portMAX_DELAY 0
typedef struct {
    u8g2_t u8g2;
    u8x8_display_info_t info;
    solar_os_rgb_panel_buffer_t image;
    uint32_t mono_hashes[52];
    uint64_t mono_valid_rows;
    const uint8_t *surface_data;
    bool frame_cached;
    void *mutex;
    uint16_t foreground, background;
    bool ready;
    esp_err_t last_error;
} solar_os_spd2010_t;
static int locked, transfers;
static void xSemaphoreTake(void *m, int delay) { (void)m; (void)delay; assert(!locked++); }
static void xSemaphoreGive(void *m) { (void)m; assert(locked-- == 1); }
void u8x8_d_helper_display_setup_memory(u8x8_t *u, const u8x8_display_info_t *info) { u->display_info=info; }
static esp_err_t solar_os_spd2010_power_save(solar_os_spd2010_t *d, bool enabled) { (void)d; (void)enabled; return ESP_OK; }
static esp_err_t flush_changed_rows(solar_os_spd2010_t *d) {
    assert(d && locked); transfers++; return ESP_OK;
}
''' + source + r'''
int main(void) {
    solar_os_spd2010_t d={.ready=true,.foreground=0x1234,.background=0xfedc,
        .image={.width=WIDTH,.height=HEIGHT}};
    d.image.pixels=malloc(WIDTH*HEIGHT*2U); assert(d.image.pixels);
    memset(d.image.pixels,0xa5,WIDTH*HEIGHT*2U);
    uint8_t columns[8]={1,2,4,8,16,32,64,128};
    u8x8_tile_t tile={.x_pos=3,.y_pos=5,.cnt=1,.tile_ptr=columns};
    assert(display_callback(&d.u8g2.u8x8,U8X8_MSG_DISPLAY_DRAW_TILE,1,&tile));
    for (unsigned y=0;y<8;y++) for (unsigned x=0;x<8;x++) {
        uint8_t *p=d.image.pixels+((40+y)*WIDTH+24+x)*2U;
        /* The OS emits one for white/background, zero for black/foreground. */
        assert((((uint16_t)p[0]<<8)|p[1])==(x==y ? d.background : d.foreground));
    }
    assert(!transfers && !locked);
    assert(display_callback(&d.u8g2.u8x8,U8X8_MSG_DISPLAY_REFRESH,0,NULL));
    assert(transfers==1 && !locked);
    free(d.image.pixels);
}
''', "-I", str(ROOT / "components/u8g2/src/clib"), "-I", str(ROOT / "src/drivers"),
              "-I", str(ROOT / "src/services"),
              "-fsanitize=address,undefined", "-no-pie")

    def test_changed_rows_dma_completion_and_failed_transfer_retry(self):
        source = production_functions((ROOT / "src/drivers/spd2010.c").read_text(),
            ("flush_rows", "rows_dirty", "flush_changed_rows", "from_u8x8", "hash_bytes", "display_callback",
             "solar_os_spd2010_frame", "solar_os_spd2010_surface", "solar_os_spd2010_colors"))
        fixture = (ROOT / "tests/host/spd2010_refresh_test.c").read_text()
        run_c(fixture.replace("/* PRODUCTION_CALLBACKS */", source),
              "-I", str(ROOT / "components/u8g2/src/clib"), "-I", str(ROOT / "src/drivers"),
              "-I", str(ROOT / "src/services"), str(ROOT / "src/drivers/solar_os_rgb_panel_buffer.c"),
              "-fsanitize=address,undefined", "-no-pie")

    def test_supplier_startup_order_and_failure_keeps_backlight_off(self):
        source = production_functions((ROOT / "src/drivers/spd2010.c").read_text(),
                                      ("solar_os_spd2010_resume",))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#define HEIGHT 412
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(ms) (ms)
typedef struct {
    struct { bool have_power, power_active_high, have_reset, invert; int power, reset; } config;
    void *panel, *mutex;
    bool ready, sleeping, io_failed;
    esp_err_t last_error;
    uint8_t dirty_rows[52];
    uint64_t mono_valid_rows;
    const uint8_t *surface_data;
    bool frame_cached;
} solar_os_spd2010_t;
static int stage, locked, lights;
static bool inversion;
static esp_err_t init_error, mirror_error;
static void xSemaphoreTake(void *m, int delay) { (void)m; (void)delay; assert(!locked++); }
static void xSemaphoreGive(void *m) { (void)m; assert(locked-- == 1); }
static void vTaskDelay(int ms) { assert(ms > 0); }
static esp_err_t backlight(solar_os_spd2010_t *d, bool on) {
    assert(d && locked);
    if (on) { assert(stage == (inversion ? 7 : 6)); lights++; }
    else { assert(stage++ == 0); lights = 0; }
    return ESP_OK;
}
static esp_err_t control(const int *line, bool high) { assert(*line == 9 && high && stage++ == 1); return ESP_OK; }
static esp_err_t esp_lcd_panel_reset(void *panel) { assert(panel && stage++ == 2); return ESP_OK; }
static esp_err_t esp_lcd_panel_init(void *panel) { assert(panel && stage++ == 3); return init_error; }
static esp_err_t esp_lcd_panel_mirror(void *panel, bool x, bool y) { assert(panel && !x && !y && stage++ == 4); return mirror_error; }
static esp_err_t esp_lcd_panel_invert_color(void *panel, bool invert) { assert(panel && inversion && invert && stage++ == 5); return ESP_OK; }
static esp_err_t flush_rows(solar_os_spd2010_t *d, uint16_t first, uint16_t end) {
    assert(d && first == 0 && end == 412 && stage++ == (inversion ? 6 : 5)); return ESP_OK;
}
static esp_err_t esp_lcd_panel_disp_on_off(void *panel, bool on) {
    assert(panel && on && stage == (inversion ? 7 : 6)); return ESP_OK;
}
''' + source + r'''
int main(void) {
    int panel;
    solar_os_spd2010_t d = {.panel = &panel,
        .config = {.have_power = true, .power = 9, .power_active_high = true}};
    for (int invert = 0; invert < 2; invert++) {
        stage = 0; inversion = d.config.invert = invert;
        assert(solar_os_spd2010_resume(&d) == ESP_OK);
        assert(d.ready && !d.sleeping && lights == 1 && !locked);
    }
    stage = 0; init_error = ESP_FAIL;
    assert(solar_os_spd2010_resume(&d) == ESP_FAIL && !d.ready && !lights && stage == 4 && !locked);
    stage = 0; init_error = ESP_OK; mirror_error = ESP_FAIL;
    assert(solar_os_spd2010_resume(&d) == ESP_FAIL && !d.ready && !lights && stage == 5 && !locked);
}
''')

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
