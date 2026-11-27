#include "display_canvas_fixture.h"

static void geometry(void)
{
    solar_os_display_geometry_t g;
    assert(sizeof(g) <= 64); /* Geometry never duplicates the framebuffer. */
    solar_os_display_geometry_init(&g,412,412,true);
    assert(g.canvas.x==60 && g.canvas.y==60 && g.canvas.width==291 && g.canvas.height==291);
    solar_os_display_geometry_init(&g,800,480,false);
    assert(g.canvas.x==0 && g.canvas.y==0 && g.canvas.width==800 && g.canvas.height==480);
    const solar_os_display_rect_t rectangle={10,20,100,60};
    assert(solar_os_display_geometry_set_canvas(&g,&rectangle));
    solar_os_display_geometry_update(&g,90,1);
    assert(g.panel.width==480 && g.panel.height==800);
    assert(g.canvas.x==400 && g.canvas.y==10 && g.canvas.width==60 && g.canvas.height==100);
    for (unsigned r=0;r<360;r+=90) for (unsigned scale=1;scale<=4;scale++) {
        solar_os_display_geometry_update(&g,r,scale);
        const solar_os_display_rect_t c={1,2,30,20};
        assert(solar_os_display_geometry_set_canvas(&g,&c));
        assert(!memcmp(&g.canvas,&c,sizeof(c)));
        solar_os_display_geometry_t previous=g;
        const solar_os_display_rect_t bad[]={ {-1,0,1,1},{0,0,0,1},{0,0,1,0},
            {0,0,10000,1},{g.panel.width,0,1,1},{0,g.panel.height,1,1} };
        for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) {
            assert(!solar_os_display_geometry_set_canvas(&g,bad+i));
            assert(!memcmp(&g,&previous,sizeof(g)));
        }
        solar_os_display_geometry_update(&g,(r+90)%360,scale);
        solar_os_display_geometry_update(&g,r,scale);
        assert(!memcmp(&g.canvas,&c,sizeof(c)));
        assert(solar_os_display_geometry_set_canvas(&g,NULL));
        assert(g.canvas.width==g.panel.width && g.canvas.height==g.panel.height);
    }
    solar_os_display_geometry_init(&g,413,413,false);
    const solar_os_display_rect_t edge={412,412,1,1};
    assert(solar_os_display_geometry_set_canvas(&g,&edge));
    solar_os_display_geometry_update(&g,0,4);
    assert(g.canvas.x+g.canvas.width<=g.panel.width && g.canvas.y+g.canvas.height<=g.panel.height);
}

static void packed_icons(solar_os_gfx_t *gfx)
{
    const uint8_t *original[] = {u8g2_font_open_iconic_all_4x_t,
        u8g2_font_open_iconic_all_6x_t, u8g2_font_open_iconic_all_8x_t};
    uint8_t expected[52 * 52 * 8];
    u8g2_t *u = gfx->u8g2;
    unsigned allocations_before = allocations;
    for (unsigned face = 0; face < sizeof(original) / sizeof(original[0]); ++face) {
        unsigned size = 32 + 16 * face;
        for (unsigned icon = 0; icon < SOLAR_OS_GFX_ICON_COUNT; ++icon) {
            u8g2_ClearBuffer(u);
            gfx_draw_icon(gfx, -10, 3, size, original[face], GFX_OPEN_ICONIC_FIRST_ENCODING + icon);
            memcpy(expected, u->tile_buf_ptr, sizeof(expected));
            u8g2_ClearBuffer(u);
            solar_os_gfx_icon(gfx, -10, 3, icon, size);
            assert(!memcmp(expected, u->tile_buf_ptr, sizeof(expected)));
            assert(u->font == gfx_font_data(gfx->font));
        }
    }
    assert(allocations == allocations_before);
}

static void pixels(void)
{
    uint8_t storage[16+52*52*8+16]; memset(storage,0xa5,sizeof(storage));
    u8g2_t u; memset(&u,0xa5,sizeof(u));
    u8g2_SetupDisplay(&u,display,u8x8_dummy_cb,u8x8_dummy_cb,u8x8_dummy_cb);
    assert(!u8g2_GetUserPtr(&u));
    u8g2_SetupBuffer(&u,storage+16,52,u8g2_ll_hvline_vertical_top_lsb,U8G2_R0);
    solar_os_display_geometry_t g;
    solar_os_display_geometry_init(&g,412,412,true); u8g2_SetUserPtr(&u,&g);
    solar_os_gfx_t gfx; solar_os_gfx_init(&gfx,&u);
    solar_os_gfx_set_black_is_one(&gfx,true);
    const solar_os_gfx_point_t large_triangle[] = {
        {INT16_MIN, INT16_MIN}, {INT16_MAX, INT16_MAX}, {INT16_MIN, INT16_MAX},
    };
    solar_os_gfx_clear(&gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_color(&gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_polygon(&gfx, large_triangle, 3);
    for (unsigned y = 0; y < 412; ++y)
        for (unsigned x = 0; x < 412; ++x) assert(mono_pixel(&u, x, y) == (x <= y));
    assert(solar_os_gfx_width(&gfx)==291 && solar_os_gfx_height(&gfx)==291);
    solar_os_display_rect_t panel=solar_os_gfx_panel_bounds(&gfx);
    assert(panel.x==-60 && panel.y==-60 && panel.width==412 && panel.height==412);
    const uint8_t rgb[]={0x12,0x34};
    solar_os_display_raster_t frame={.data=rgb,.data_size=2,.source_width=1,.source_height=1,
        .source_stride=2,.width=1,.height=1,.format=SOLAR_OS_DISPLAY_FORMAT_RGB565};
    assert(solar_os_gfx_present_frame_at(&gfx,&frame,panel.x,panel.y)==ESP_OK);
    assert(!presented.x && !presented.y && presented.data==rgb);
    assert(solar_os_gfx_present_frame(&gfx,&frame)==ESP_OK && presented.x==60 && presented.y==60);
    assert(solar_os_gfx_present_frame_at(&gfx,&frame,351,351)==ESP_OK && presented.x==411 && presented.y==411);
    assert(solar_os_gfx_present_frame_at(&gfx,&frame,-61,0)==ESP_ERR_INVALID_ARG);
    assert(solar_os_gfx_present_frame_at(&gfx,&frame,INT_MAX,INT_MIN)==ESP_ERR_INVALID_ARG);
    packed_icons(&gfx);
    for(unsigned indexed=0;indexed<2;indexed++) {
        if(indexed) assert(solar_os_gfx_enable_index8(&gfx)==ESP_OK);
        solar_os_gfx_clear(&gfx,SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_set_color(&gfx,SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_pixel(&gfx,0,0);
        solar_os_gfx_pixel(&gfx,-60,146); /* Left edge outside canvas, inside panel. */
        solar_os_gfx_pixel(&gfx,351,146); /* Right edge outside canvas. */
        solar_os_gfx_pixel(&gfx,146,-60);
        solar_os_gfx_pixel(&gfx,146,351);
        solar_os_gfx_fill_rect(&gfx,292,146,3,2);
        if(indexed) {
            uint8_t ink=gfx.index8->draw_index;
            assert(gfx.index8->pixels[60*412+60]==ink);
            assert(gfx.index8->pixels[206*412]==ink && gfx.index8->pixels[206*412+411]==ink);
            assert(gfx.index8->pixels[206]==ink && gfx.index8->pixels[411*412+206]==ink);
            assert(gfx.index8->pixels[206*412+352]==ink);
        } else {
            assert(mono_pixel(&u,60,60) && !mono_pixel(&u,0,0));
            assert(mono_pixel(&u,0,206) && mono_pixel(&u,411,206));
            assert(mono_pixel(&u,206,0) && mono_pixel(&u,206,411));
            assert(mono_pixel(&u,352,206));
        }
        /* Text, icons, rectangles and bitmaps use the same canvas origin. */
        solar_os_gfx_rect(&gfx,-10,20,12,10);
        solar_os_gfx_text(&gfx,-10,60,"Canvas");
        solar_os_gfx_icon(&gfx,-10,65,SOLAR_OS_GFX_ICON_TABLET,SOLAR_OS_GFX_ICON_SIZE_16);
        const uint8_t bitmap[]={1};
        solar_os_gfx_bitmap(&gfx,-60,147,1,1,bitmap);
        const uint8_t gray[]={0};
        solar_os_gfx_raster_t raster={.pixels=gray,.pixels_size=1,.width=1,.height=1,
            .stride=1,.format=SOLAR_OS_GFX_RASTER_GRAY8};
        assert(solar_os_gfx_blit_raster(&gfx,&raster,-60,148,1,1,NULL)==ESP_OK);
        solar_os_gfx_color_t colors[4]={SOLAR_OS_GFX_COLOR_BLACK};
        const uint8_t packed[]={0};
        solar_os_gfx_bitmap_2bpp(&gfx,-60,149,1,1,packed,1,colors);
        if(indexed) {
            uint8_t ink=gfx.index8->draw_index;
            assert(gfx.index8->pixels[207*412]==ink && gfx.index8->pixels[209*412]==ink);
        } else assert(mono_pixel(&u,0,207) && mono_pixel(&u,0,208) && mono_pixel(&u,0,209));
        unsigned before=allocations;
        const solar_os_display_rect_t smaller={100,110,160,160};
        assert(solar_os_display_geometry_set_canvas(&g,&smaller));
        assert(solar_os_gfx_width(&gfx)==160 && solar_os_gfx_height(&gfx)==160);
        solar_os_gfx_clear(&gfx,SOLAR_OS_GFX_COLOR_WHITE);
        if(indexed) assert(gfx.index8->pixels[206*412]!=gfx.index8->draw_index);
        else assert(!mono_pixel(&u,0,206) && !mono_pixel(&u,60,60));
        assert(solar_os_display_geometry_set_canvas(&g,NULL));
        assert(allocations==before);
    }
    solar_os_gfx_release_surface(&gfx);
    /* Terminal primitives keep their grid coordinates and shift at drawing. */
    u8g2_ClearBuffer(&u); u8g2_SetDrawColor(&u,1);
    solar_os_canvas_box(&u,0,0,8,8);
    assert(mono_pixel(&u,60,60) && mono_pixel(&u,67,67) && !mono_pixel(&u,59,60));
    for(unsigned i=0;i<16;i++) assert(storage[i]==0xa5 && storage[16+52*52*8+i]==0xa5);
}

int main(void) { geometry(); pixels(); return 0; }
