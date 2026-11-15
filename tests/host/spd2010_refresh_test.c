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
#define ROW_ALIGNMENT 4U
#define portMAX_DELAY 1
typedef struct {
    u8g2_t u8g2;
    u8x8_display_info_t info;
    solar_os_rgb_panel_buffer_t image;
    uint8_t dirty_rows[(HEIGHT+7)/8];
    uint32_t mono_hashes[(HEIGHT+7)/8];
    uint64_t mono_valid_rows;
    const uint8_t *surface_data;
    uint32_t surface_palette_hash;
    uint16_t surface_stride;
    solar_os_display_rotation_t surface_rotation;
    uint32_t frame_hashes[(HEIGHT+7)/8];
    uint64_t frame_valid_rows;
    uint32_t frame_palette_hash;
    uint16_t frame_width, frame_height;
    solar_os_display_rotation_t frame_rotation;
    bool frame_cached, frame_inverted;
    void *mutex, *done, *panel, *io;
    uint16_t foreground, background;
    bool ready, io_failed;
    esp_err_t last_error;
} solar_os_spd2010_t;

static solar_os_spd2010_t *active;
static unsigned transfers, bytes, last_first, last_end, fail_at, drains;
static esp_err_t transfer_error=ESP_ERR_NO_MEM;
static bool locked, pending;
static int xSemaphoreTake(void *s, int wait)
{
    if (s == active->mutex) { assert(!locked); locked=true; return 1; }
    assert(s==active->done);
    if (!wait) { assert(!pending); return 0; }
    assert(pending); pending=false; return 1;
}
static void xSemaphoreGive(void *s) { assert(s==active->mutex && locked); locked=false; }
void u8x8_d_helper_display_setup_memory(u8x8_t *u, const u8x8_display_info_t *info) { u->display_info=info; }
static esp_err_t solar_os_spd2010_power_save(solar_os_spd2010_t *d, bool on) { (void)d; (void)on; return ESP_OK; }
static solar_os_display_rotation_t test_rotation;
static solar_os_display_rotation_t rotation(const solar_os_spd2010_t *d) { (void)d; return test_rotation; }
static esp_err_t esp_lcd_panel_draw_bitmap(void *panel, int x, int y, int end_x, int end_y, const void *data)
{
    assert(panel==active->panel && !pending && x==0 && end_x==WIDTH);
    assert(y>=0 && (unsigned)end_y<=HEIGHT && end_y>y && !(y%4) && !(end_y%4));
    assert(data==active->image.pixels+(size_t)y*WIDTH*2);
    transfers++; last_first=y; last_end=end_y;
    if (fail_at==transfers) { pending=true; return transfer_error; }
    bytes+=(end_y-y)*WIDTH*2; pending=true; return ESP_OK;
}
static esp_err_t esp_lcd_panel_io_tx_param(void *io, int command, const void *data, size_t size)
{
    assert(io==active->io && command==-1 && !data && !size && locked);
    if (pending) { pending=false; drains++; }
    return ESP_OK;
}

/* PRODUCTION_CALLBACKS */

static void refresh(solar_os_spd2010_t *d)
{
    assert(display_callback(&d->u8g2.u8x8,U8X8_MSG_DISPLAY_REFRESH,0,NULL));
    assert(!locked && !pending);
}

int main(void)
{
    solar_os_spd2010_t d={.ready=true,.foreground=0,.background=0xffff,
        .mutex=(void *)1,.done=(void *)2,.panel=(void *)3,.io=(void *)4};
    active=&d;
    uint8_t *storage=malloc(WIDTH*HEIGHT*2+32); assert(storage);
    memset(storage,0xa5,WIDTH*HEIGHT*2+32);
    d.image=(solar_os_rgb_panel_buffer_t){.pixels=storage+16,.width=WIDTH,.height=HEIGHT,.dirty_rows=d.dirty_rows};
    memset(d.image.pixels,0,WIDTH*HEIGHT*2);
    uint8_t columns[8]; memset(columns,0xff,sizeof(columns));
    u8x8_tile_t tile={.cnt=1,.tile_ptr=columns};
    assert(display_callback(&d.u8g2.u8x8,U8X8_MSG_DISPLAY_DRAW_TILE,1,&tile));
    assert(!transfers); /* Conversion batches until the U8g2 refresh boundary. */
    refresh(&d); assert(transfers==1 && last_first==0 && last_end==8);
    assert(display_callback(&d.u8g2.u8x8,U8X8_MSG_DISPLAY_DRAW_TILE,1,&tile));
    refresh(&d); assert(transfers==1); /* Identical text needs no SPI transfer. */

    transfers=bytes=0;
    solar_os_rgb_panel_clear(&d.image,0xffff); refresh(&d);
    assert(transfers==1 && bytes==WIDTH*HEIGHT*2);
    solar_os_rgb_panel_clear(&d.image,0xffff); refresh(&d); assert(transfers==1);
    transfers=bytes=0;
    const uint8_t pixel[]={0x12,0x34};
    solar_os_display_raster_t f={.data=pixel,.data_size=2,.source_width=1,.source_height=1,
        .source_stride=2,.x=60,.y=60,.width=12,.height=12,.format=SOLAR_OS_DISPLAY_FORMAT_RGB565};
    for (unsigned rotation=0;rotation<4;rotation++) {
        assert(solar_os_rgb_panel_frame(&d.image,&f,rotation)==ESP_OK); refresh(&d);
        assert(transfers==rotation+1 && last_first==(rotation<2 ? 60U : 340U));
        assert(last_end==last_first+12);
    }
    assert(bytes==4*WIDTH*12*2);
    assert(solar_os_rgb_panel_frame(&d.image,&f,3)==ESP_OK); refresh(&d); assert(transfers==4);

    transfers=bytes=0; fail_at=2;
    solar_os_rgb_panel_put(&d.image,0,2,0);
    solar_os_rgb_panel_put(&d.image,0,410,0);
    assert(!display_callback(&d.u8g2.u8x8,U8X8_MSG_DISPLAY_REFRESH,0,NULL));
    assert(d.last_error==ESP_ERR_NO_MEM && transfers==2 && drains==1 && !locked && !pending);
    fail_at=0; refresh(&d); assert(transfers==3 && last_first==408 && last_end==412);
    refresh(&d); assert(transfers==3);

    solar_os_rgb_panel_clear(&d.image,0xffff); refresh(&d);
    uint8_t *indices=calloc(WIDTH*HEIGHT,1); assert(indices);
    uint16_t palette[256];
    for (unsigned i=0;i<256;i++) palette[i]=0xffff;
    palette[1]=0xabcd; indices[410*WIDTH+410]=1;
    solar_os_display_surface_t s={.data=indices,.data_size=WIDTH*HEIGHT,
        .width=WIDTH,.height=HEIGHT,.stride=WIDTH,.native_width=WIDTH,.native_height=HEIGHT,
        .format=SOLAR_OS_DISPLAY_FORMAT_INDEX8,.palette_rgb565=palette,.palette_size=256};
    const unsigned expected_transfers[]={1,1,2,1};
    for (unsigned rotation=0;rotation<4;rotation++) {
        const unsigned before=transfers;
        s.rotation=rotation;
        assert(solar_os_rgb_panel_surface(&d.image,&s)==ESP_OK); refresh(&d);
        assert(transfers==before+expected_transfers[rotation]);
    }
    unsigned before=transfers;
    assert(solar_os_rgb_panel_surface(&d.image,&s)==ESP_OK); refresh(&d); assert(transfers==before);
    palette[1]=0xbeef;
    assert(solar_os_rgb_panel_surface(&d.image,&s)==ESP_OK); refresh(&d); assert(transfers==before+1);
    free(indices);

    /* The right/bottom partial tile never writes beyond the actual 412 pixels. */
    tile.x_pos=51; tile.y_pos=51;
    assert(display_callback(&d.u8g2.u8x8,U8X8_MSG_DISPLAY_DRAW_TILE,1,&tile));
    refresh(&d);
    for (unsigned i=0;i<16;i++) assert(storage[i]==0xa5 && storage[16+WIDTH*HEIGHT*2+i]==0xa5);

    /* Only dirty native tiles are converted after the first indexed surface. */
    indices=calloc(WIDTH*HEIGHT,1); assert(indices);
    uint8_t dirty[52*7]; memset(dirty,0xff,sizeof(dirty));
    uint32_t *hashes=calloc(52*52,sizeof(uint32_t)); assert(hashes);
    s.data=indices; s.rotation=0; s.dirty_tiles=dirty; s.dirty_stride=7;
    s.dirty_size=sizeof(dirty); s.tile_size=8;
    s.presented_hashes=hashes; s.hash_stride=52; s.presented_hash_count=52*52;
    assert(solar_os_spd2010_surface(&d,&s)==ESP_OK); /* First surface always converts all pixels. */
    assert(solar_os_rgb_panel_surface_update(&d.image,&s,false)==ESP_OK);
    /* A full-redraw app marks every tile dirty, but unchanged final pixels
     * still skip RGB conversion using the surface's existing hash storage. */
    memset(dirty,0xff,sizeof(dirty)); d.image.pixels[0]=0x12;
    assert(solar_os_rgb_panel_surface_update(&d.image,&s,true)==ESP_OK && d.image.pixels[0]==0x12);
    assert(solar_os_spd2010_surface(&d,&s)==ESP_OK && d.frame_cached);
    d.image.pixels[0]=0x12;
    assert(solar_os_spd2010_surface(&d,&s)==ESP_OK && d.image.pixels[0]==0x12);
    d.surface_data=NULL; d.frame_cached=false;
    assert(solar_os_spd2010_surface(&d,&s)==ESP_OK && d.image.pixels[0]==0xff);
    memset(dirty,0,sizeof(dirty));
    assert(solar_os_spd2010_surface(&d,&s)==ESP_OK && d.surface_data==s.data);
    indices[410*WIDTH+410]=1;
    indices[100*WIDTH+100]=1; /* Unmarked source content must not be visited. */
    dirty[51*7+6]=1U<<3;
    before=transfers;
    assert(solar_os_spd2010_surface(&d,&s)==ESP_OK);
    assert(transfers==before+1 && last_first==408 && last_end==412);
    assert(d.image.pixels[(100*WIDTH+100)*2]==0xff);
    memset(dirty,0,sizeof(dirty));
    before=transfers;
    assert(solar_os_spd2010_surface(&d,&s)==ESP_OK); assert(transfers==before);
    /* Palette edits and another presentation invalidate the cached surface. */
    palette[1]=0x4567;
    memset(dirty,0xff,sizeof(dirty));
    assert(solar_os_spd2010_surface(&d,&s)==ESP_OK);
    assert(d.image.pixels[(100*WIDTH+100)*2]==0x45);
    f=(solar_os_display_raster_t){.data=pixel,.data_size=2,.source_width=1,.source_height=1,
        .source_stride=2,.width=1,.height=1,.format=SOLAR_OS_DISPLAY_FORMAT_RGB565};
    assert(solar_os_spd2010_frame(&d,&f)==ESP_OK && !d.surface_data);
    /* A status-row patch after a direct RGB565 frame preserves the video. */
    memset(dirty,0,sizeof(dirty)); dirty[20*7]=1;
    assert(solar_os_spd2010_surface(&d,&s)==ESP_OK && d.image.pixels[0]==0x12);
    /* Clearing the whole graphics canvas must still erase the old video. */
    memset(dirty,0xff,sizeof(dirty));
    assert(solar_os_spd2010_surface(&d,&s)==ESP_OK && d.image.pixels[0]==0xff);
    /* Native dirty tiles map correctly for all four rotations, including edge tiles. */
    for (unsigned r=0;r<4;r++) {
        s.rotation=r;
        memset(dirty,0xff,sizeof(dirty));
        assert(solar_os_spd2010_surface(&d,&s)==ESP_OK);
        indices[410*WIDTH+410]=indices[410*WIDTH+410] ? 0 : 1;
        unsigned nx=(r==0 || r==3) ? 410 : 1;
        unsigned ny=r<2 ? 410 : 1;
        memset(dirty,0,sizeof(dirty)); dirty[(ny/8)*7+(nx/8)/8]=1U<<((nx/8)&7);
        assert(solar_os_spd2010_surface(&d,&s)==ESP_OK);
        uint8_t *p=d.image.pixels+(ny*WIDTH+nx)*2;
        assert(p[0]==(indices[410*WIDTH+410] ? 0x45 : 0xff));
    }
    /* A malformed dirty map falls back to full conversion. */
    s.dirty_size=0; indices[0]=1;
    assert(solar_os_spd2010_surface(&d,&s)==ESP_OK);
    assert(d.image.pixels[(411*WIDTH)*2]==0x45);
    free(indices); free(hashes);

    /* Whole-row text caching skips identical conversion; partial writes, color
     * changes and indexed/RGB presentation invalidate it. */
    uint8_t full_row[WIDTH]; memset(full_row,0xff,sizeof(full_row));
    tile=(u8x8_tile_t){.cnt=52,.tile_ptr=full_row};
    assert(display_callback(&d.u8g2.u8x8,U8X8_MSG_DISPLAY_DRAW_TILE,1,&tile)); refresh(&d);
    assert(d.mono_valid_rows&1);
    d.image.pixels[0]=0x12; /* Sentinel detects whether the converter runs. */
    assert(display_callback(&d.u8g2.u8x8,U8X8_MSG_DISPLAY_DRAW_TILE,1,&tile));
    assert(d.image.pixels[0]==0x12);
    assert(solar_os_spd2010_colors(&d,0,0xffffff)==ESP_OK && !d.mono_valid_rows);
    assert(display_callback(&d.u8g2.u8x8,U8X8_MSG_DISPLAY_DRAW_TILE,1,&tile));
    assert(d.image.pixels[0]==0xff);
    tile.cnt=1;
    assert(display_callback(&d.u8g2.u8x8,U8X8_MSG_DISPLAY_DRAW_TILE,1,&tile));
    assert(!(d.mono_valid_rows&1));
    tile.cnt=52;
    assert(display_callback(&d.u8g2.u8x8,U8X8_MSG_DISPLAY_DRAW_TILE,1,&tile));
    assert(d.mono_valid_rows&1);
    assert(solar_os_spd2010_frame(&d,&f)==ESP_OK && !d.mono_valid_rows);

    /* Scaled full-panel INDEX8 rasters preserve row caching across source
     * buffer swaps, and invalidate it on palette/rotation/format changes. */
    uint8_t *scaled=calloc(206*206,1); assert(scaled);
    f=(solar_os_display_raster_t){.data=scaled,.data_size=206*206,
        .source_width=206,.source_height=206,.source_stride=206,
        .width=WIDTH,.height=HEIGHT,.format=SOLAR_OS_DISPLAY_FORMAT_INDEX8,
        .palette_rgb565=palette,.palette_size=256,.clear_background=true};
    assert(solar_os_spd2010_frame(&d,&f)==ESP_OK && d.frame_cached);
    before=transfers;
    assert(solar_os_spd2010_frame(&d,&f)==ESP_OK && transfers==before);
    scaled[205*206+205]=1;
    assert(solar_os_spd2010_frame(&d,&f)==ESP_OK);
    assert(transfers==before+1 && last_first==408 && last_end==412);
    uint8_t *alternate=malloc(206*206); assert(alternate); memcpy(alternate,scaled,206*206);
    f.data=alternate; before=transfers;
    assert(solar_os_spd2010_frame(&d,&f)==ESP_OK && transfers==before);
    palette[1]=0x1234;
    assert(solar_os_spd2010_frame(&d,&f)==ESP_OK && transfers==before+1);
    for (unsigned r=0;r<4;r++) {
        test_rotation=r;
        assert(solar_os_spd2010_frame(&d,&f)==ESP_OK);
        unsigned nx=(r==0 || r==3) ? 410 : 0;
        unsigned ny=r<2 ? 410 : 0;
        assert(d.image.pixels[(ny*WIDTH+nx)*2]==0x12);
    }
    f.data_size=1;
    assert(solar_os_spd2010_frame(&d,&f)==ESP_ERR_INVALID_ARG && !d.frame_cached);
    test_rotation=0;
    memset(alternate,0,103*103); alternate[102*103+102]=1;
    f.source_width=f.source_height=f.source_stride=103; f.data_size=103*103;
    assert(solar_os_spd2010_frame(&d,&f)==ESP_OK);
    assert(d.image.pixels[(408*WIDTH+408)*2]==0x12);
    before=transfers;
    assert(solar_os_spd2010_frame(&d,&f)==ESP_OK && transfers==before);
    for (unsigned i=0;i<16;i++) assert(storage[i]==0xa5 && storage[16+WIDTH*HEIGHT*2+i]==0xa5);
    free(alternate); free(scaled);
    /* A failed SPI completion must not block draining a stale SDK count, nor
     * allow the still-retained image to be overwritten by another frame. */
    solar_os_rgb_panel_put(&d.image,0,0,0);
    fail_at=transfers+1; transfer_error=ESP_FAIL;
    assert(!display_callback(&d.u8g2.u8x8,U8X8_MSG_DISPLAY_REFRESH,0,NULL));
    assert(d.io_failed && !d.ready && !locked && pending && drains==1);
    assert(solar_os_spd2010_frame(&d,&f)==ESP_ERR_INVALID_STATE);
    pending=false; /* Simulated hardware completion before fixture teardown. */
    free(storage);
}
