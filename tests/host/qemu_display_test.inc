#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "u8g2.h"
#include "solar_os_indexed_raster.h"
#define EXT_RAM_BSS_ATTR
#define SOLAR_OS_EXPANSION_DEVICE_NAME_MAX 32
typedef void *esp_lcd_panel_handle_t;
typedef struct { int unused; } solar_os_board_display_t;
const u8g2_cb_t u8g2_cb_r0 = {0}, u8g2_cb_r1 = {0}, u8g2_cb_r2 = {0}, u8g2_cb_r3 = {0};
static unsigned refresh_count;
static esp_err_t refresh_result;
static esp_err_t esp_lcd_rgb_qemu_refresh(esp_lcd_panel_handle_t panel)
{ assert(panel); refresh_count++; return refresh_result; }
/* FUNCTIONS */
static const u8g2_cb_t *rotations[] = {U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3};
static size_t native_offset(unsigned x, unsigned y, unsigned rotation)
{
    unsigned nx, ny;
    switch (rotation) {
    case 1: nx = QEMU_RGB_WIDTH - 1 - y; ny = x; break;
    case 2: nx = QEMU_RGB_WIDTH - 1 - x; ny = QEMU_RGB_HEIGHT - 1 - y; break;
    case 3: nx = y; ny = QEMU_RGB_HEIGHT - 1 - x; break;
    default: nx = x; ny = y;
    }
    return ny * QEMU_RGB_WIDTH + nx;
}
int main(void)
{
    const size_t pixels = QEMU_RGB_WIDTH * QEMU_RGB_HEIGHT;
    uint16_t *guarded = malloc((pixels + 2) * sizeof(uint16_t)); assert(guarded);
    guarded[0] = guarded[pixels+1] = 0x5a5a;
    device.panel = (void *)1; device.framebuffer = guarded + 1;
    device.background = 0xffff; device.foreground = 0;
    solar_os_board_display_t display = {0};
    assert(rgb565(0xff0000) == 0xf800 && rgb565(0x00ff00) == 0x07e0 && rgb565(0x0000ff) == 0x001f);
    /* RGB wire byte order, scaling, retained regions, clear and every rotation. */
    const uint8_t rgb[] = {0xf8,0x00, 0x07,0xe0, 0x00,0x1f, 0xff,0xff};
    solar_os_display_raster_t frame = {
        .data=rgb, .data_size=sizeof(rgb), .source_width=2, .source_height=2, .source_stride=4,
        .x=1, .y=1, .width=4, .height=4, .format=SOLAR_OS_DISPLAY_FORMAT_RGB565,
    };
    const uint16_t expected[] = {0xf800, 0x07e0, 0x001f, 0xffff};
    for (unsigned rotation = 0; rotation < 4; rotation++) {
        device.u8g2.cb = rotations[rotation];
        for (size_t i = 0; i < pixels; i++) device.framebuffer[i] = 0x1234;
        assert(present_raster(&frame) == ESP_OK);
        assert(device.framebuffer[native_offset(0,0,rotation)] == 0x1234);
        for (unsigned y=0; y<4; y++) for (unsigned x=0; x<4; x++)
            assert(device.framebuffer[native_offset(x+1,y+1,rotation)] == expected[y/2*2+x/2]);
        frame.clear_background = true; frame.background_rgb565 = 0x4321;
        assert(present_raster(&frame) == ESP_OK);
        assert(device.framebuffer[native_offset(0,0,rotation)] == 0x4321);
        frame.clear_background = false;
    }
    device.u8g2.cb = U8G2_R0;
    /* Validation must reject malformed frames before modifying any pixel. */
    solar_os_display_raster_t invalid = frame;
    invalid.source_stride = 3; assert(present_raster(&invalid) == ESP_ERR_INVALID_ARG);
    invalid = frame; invalid.data_size--; assert(present_raster(&invalid) == ESP_ERR_INVALID_ARG);
    invalid = frame; invalid.palette_inverted = true; assert(present_raster(&invalid) == ESP_ERR_INVALID_ARG);
    invalid = frame; invalid.x = QEMU_RGB_WIDTH-1; assert(present_raster(&invalid) == ESP_ERR_INVALID_SIZE);
    assert(present_raster(NULL) == ESP_ERR_INVALID_ARG);
    /* Packed INDEX2 order, arbitrary INDEX8 palette and their distinct inversion. */
    uint16_t palette[256]; for (unsigned i=0; i<256; i++) palette[i] = i*251;
    const uint8_t index2[] = {0xe4}, index8[] = {0, 1, 128, 255};
    frame = (solar_os_display_raster_t){
        .data=index2, .data_size=1, .source_width=4, .source_height=1, .source_stride=1,
        .width=4, .height=1, .format=SOLAR_OS_DISPLAY_FORMAT_INDEX2,
        .palette_rgb565=palette, .palette_size=4,
    };
    for (unsigned invert=0; invert<2; invert++) {
        frame.palette_inverted = invert; assert(present_raster(&frame) == ESP_OK);
        for (unsigned x=0; x<4; x++) assert(device.framebuffer[x] == palette[invert ? 3-x : x]);
    }
    frame.format=SOLAR_OS_DISPLAY_FORMAT_INDEX8; frame.data=index8;
    frame.data_size=frame.source_stride=4; frame.palette_size=256;
    for (unsigned invert=0; invert<2; invert++) {
        frame.palette_inverted=invert; assert(present_raster(&frame) == ESP_OK);
        for (unsigned x=0; x<4; x++) {
            uint16_t want = palette[index8[x]]; if (invert) want = (uint16_t)~want;
            assert(device.framebuffer[x] == want);
        }
    }
    invalid=frame; invalid.palette_size=255; assert(present_raster(&invalid) == ESP_ERR_INVALID_ARG);
    /* One-bit mode stays black and white and advertises no color paths. */
    device.mono=true; device.mono_valid=true;
    assert(set_colors(&display,0xff0000,0x00ff00) == ESP_OK);
    assert(device.foreground == 0 && device.background == 0xffff);
    assert(present_raster(&frame) == ESP_ERR_NOT_SUPPORTED);
    const uint8_t mono[] = {0x05};
    for (unsigned invert=0; invert<2; invert++) {
        assert(present_mono_xbm(mono,1,0,0,4,1,1,invert) == ESP_OK);
        for (unsigned x=0; x<4; x++) assert(device.framebuffer[x] == (((mono[0] >> x) & 1) ^ invert ? 0 : 0xffff));
    }
    assert(set_colors(&display,0x1000000,0) == ESP_ERR_INVALID_ARG);
    device.mono=false; assert(set_colors(&display,0xff0000,0x00ff00) == ESP_OK);
    assert(device.foreground == 0xf800 && device.background == 0x07e0);
    /* U8g2 tile rows use padded width; neither padding nor final tile spills. */
    memset(draw_buffer,0,sizeof(draw_buffer));
    draw_buffer[(QEMU_RGB_TILE_HEIGHT-1)*QEMU_RGB_DRAW_STRIDE+QEMU_RGB_WIDTH-1] = 0xff;
    device.mono_valid=false; refresh_count=0;
    assert(present_draw_buffer() == ESP_OK && refresh_count == 1);
    assert(device.framebuffer[0] == device.foreground);
    assert(device.framebuffer[pixels-1] == device.background);
    assert(present_draw_buffer() == ESP_OK && refresh_count == 1);
    if (QEMU_RGB_DRAW_STRIDE > QEMU_RGB_WIDTH) {
        draw_buffer[QEMU_RGB_WIDTH] = 0xff;
        assert(present_draw_buffer() == ESP_OK && refresh_count == 1);
    }
    draw_buffer[0] ^= 1; refresh_result=ESP_FAIL;
    assert(present_draw_buffer() == ESP_FAIL && refresh_count == 2 && !device.mono_valid);
    refresh_result=ESP_OK;
    assert(present_draw_buffer() == ESP_OK && refresh_count == 3);
    assert(set_colors(&display,0x0000ff,0xffffff) == ESP_OK);
    assert(present_draw_buffer() == ESP_OK && refresh_count == 4);
    assert(device.framebuffer[QEMU_RGB_WIDTH] == 0x001f);
    /* Color app surfaces have independent rotation metadata. */
    uint8_t *data=malloc(pixels); assert(data);
    solar_os_display_surface_t surface = {
        .data=data, .data_size=pixels, .palette_rgb565=palette, .palette_size=256,
        .native_width=QEMU_RGB_WIDTH, .native_height=QEMU_RGB_HEIGHT,
        .format=SOLAR_OS_DISPLAY_FORMAT_INDEX8,
    };
    for (unsigned rotation=0; rotation<4; rotation++) {
        surface.rotation=rotation;
        surface.width=rotation&1 ? QEMU_RGB_HEIGHT : QEMU_RGB_WIDTH;
        surface.height=rotation&1 ? QEMU_RGB_WIDTH : QEMU_RGB_HEIGHT;
        surface.stride=surface.width;
        for (unsigned y=0; y<surface.height; y++) for (unsigned x=0; x<surface.width; x++)
            data[y*surface.width+x]=(x*13+y*37)&255;
        assert(present_surface(&surface) == ESP_OK);
        for (unsigned y=0; y<surface.height; y++) for (unsigned x=0; x<surface.width; x++)
            assert(device.framebuffer[native_offset(x,y,rotation)] == palette[data[y*surface.width+x]]);
    }
    surface.data_size--; assert(present_surface(&surface) == ESP_ERR_INVALID_ARG);
    device.mono=true; assert(present_surface(&surface) == ESP_ERR_NOT_SUPPORTED);
    device.panel=NULL;
    assert(present_draw_buffer() == ESP_ERR_INVALID_STATE);
    assert(present_surface(&surface) == ESP_ERR_INVALID_STATE);
    assert(present_raster(&frame) == ESP_ERR_INVALID_STATE);
    assert(guarded[0] == 0x5a5a && guarded[pixels+1] == 0x5a5a);
    free(data); free(guarded);
    printf("QEMU display %ux%u: color, mono, scaling, rotations and padded tiles passed\n",
           (unsigned)QEMU_RGB_WIDTH, (unsigned)QEMU_RGB_HEIGHT);
}
