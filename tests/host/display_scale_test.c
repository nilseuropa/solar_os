#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_display_scale.h"
#include "solar_os_memory.h"
#include "solar_os_indexed_raster.h"

static bool fail_allocation;
static unsigned refreshes, tiles;
static u8x8_display_info_t info;
static uint8_t *physical;

void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t kind, const char *tag)
{ (void)kind; (void)tag; return fail_allocation ? NULL : malloc(bytes); }
void *solar_os_memory_calloc(size_t count, size_t bytes, solar_os_memory_class_t kind, const char *tag)
{ (void)kind; (void)tag; return fail_allocation ? NULL : calloc(count, bytes); }
void solar_os_memory_free(void *pointer) { free(pointer); }

static uint8_t driver(u8x8_t *display, uint8_t message, uint8_t count, void *argument)
{
    (void)count;
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(display, &info);
    } else if (message == U8X8_MSG_DISPLAY_DRAW_TILE) {
        const u8x8_tile_t *tile = argument;
        assert(display->display_info == &info && tile->y_pos < info.tile_height);
        assert(tile->tile_ptr == physical + ((size_t)tile->y_pos * info.tile_width + tile->x_pos) * 8U);
        tiles++;
    } else if (message == U8X8_MSG_DISPLAY_REFRESH) {
        assert(display->display_info == &info); refreshes++;
    }
    return 1;
}

static bool pixel(const uint8_t *buffer, unsigned stride, unsigned x, unsigned y)
{ return (buffer[(y / 8U) * stride + x] & (1U << (y & 7U))) != 0; }

static void exercise(unsigned width, unsigned height)
{
    info = (u8x8_display_info_t){.pixel_width=width, .pixel_height=height,
        .tile_width=(width+7U)/8U, .tile_height=(height+7U)/8U};
    physical = calloc((size_t)info.tile_width * info.tile_height, 8U);
    u8g2_t canvas = {0};
    u8g2_SetupDisplay(&canvas, driver, NULL, NULL, NULL);
    u8g2_SetupBuffer(&canvas, physical, info.tile_height, u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    solar_os_display_scale_t state;
    solar_os_display_scale_init(&state, &canvas);
    fail_allocation = true;
    assert(solar_os_display_scale_prepare(&state, 2) == ESP_ERR_NO_MEM);
    assert(u8g2_GetBufferPtr(&canvas) == physical && state.factor == 1);
    fail_allocation = false;
    assert(solar_os_display_scale_prepare(&state, 0) == ESP_ERR_INVALID_ARG);
    assert(solar_os_display_scale_prepare(&state, 5) == ESP_ERR_INVALID_ARG);
    const u8g2_cb_t *rotations[] = {U8G2_R0,U8G2_R1,U8G2_R2,U8G2_R3};
    for (unsigned factor=1; factor<=4; factor++) {
        assert(solar_os_display_scale_prepare(&state, factor) == ESP_OK);
        solar_os_display_scale_apply(&state, factor);
        for (unsigned rotation=0; rotation<4; rotation++) {
            u8g2_SetDisplayRotation(&canvas, rotations[rotation]);
            unsigned w=width/factor, h=height/factor;
            assert(u8g2_GetDisplayWidth(&canvas) == (rotation&1U ? h:w));
            assert(u8g2_GetDisplayHeight(&canvas) == (rotation&1U ? w:h));
            u8g2_ClearBuffer(&canvas);
            u8g2_DrawPixel(&canvas, 1, 2);
            u8g2_DrawPixel(&canvas, u8g2_GetDisplayWidth(&canvas)-1, u8g2_GetDisplayHeight(&canvas)-1);
            unsigned stride=info.tile_width*8U, source_stride=canvas.u8x8.display_info->tile_width*8U;
            uint8_t *logical=u8g2_GetBufferPtr(&canvas);
            const size_t logical_bytes = (size_t)source_stride * u8g2_GetBufferTileHeight(&canvas);
            for (size_t i = 0; i < logical_bytes; i++) logical[i] = (uint8_t)(i * 73U + factor);
            unsigned before=refreshes;
            solar_os_display_scale_sync_native(&state);
            assert(refreshes==before);
            if(factor>1)for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++) {
                bool expected=x/factor<w && y/factor<h && pixel(logical,source_stride,x/factor,y/factor);
                assert(pixel(physical,stride,x,y)==expected);
            }
            u8g2_SendBuffer(&canvas);
            assert(refreshes==before+1 && canvas.u8x8.display_info->pixel_width==w);
            for(unsigned y=0;y<height;y++) for(unsigned x=0;x<width;x++) {
                bool expected=x/factor<w && y/factor<h && pixel(logical,source_stride,x/factor,y/factor);
                assert(pixel(physical,stride,x,y)==expected);
            }
        }
    }
    uint8_t *saved=state.logical_buffer;
    fail_allocation=true;
    assert(solar_os_display_scale_prepare(&state, 2)==ESP_OK); /* Reuses capacity. */
    fail_allocation=false;
    assert(state.logical_buffer==saved);
    solar_os_display_scale_apply(&state, 2);
    memset(physical, 0xaa, (size_t)info.tile_width * info.tile_height * 8U);
    uint8_t overlay[8]; memset(overlay, 0xff, sizeof(overlay));
    u8x8_DrawTile(&canvas.u8x8, 1, 1, 1, overlay);
    u8x8_RefreshDisplay(&canvas.u8x8);
    for (unsigned y=0; y<height; y++) for (unsigned x=0; x<width; x++) {
        bool expected = (x>=16 && x<32 && y>=16 && y<32) || (y&1U);
        assert(pixel(physical,info.tile_width*8U,x,y)==expected);
    }
    const size_t source_size=(size_t)(width/2)*(height/2);
    uint8_t *pixels=malloc(source_size); memset(pixels,17,source_size);
    solar_os_display_surface_t surface={.format=SOLAR_OS_DISPLAY_FORMAT_INDEX8,
        .data=pixels,.data_size=source_size,.native_width=width/2,.native_height=height/2,
        .width=width/2,.height=height/2,.stride=width/2};
    solar_os_display_surface_t expanded;
    fail_allocation=true;
    assert(solar_os_display_scale_surface(&state,&surface,&expanded)==ESP_ERR_NO_MEM);
    fail_allocation=false;
    assert(solar_os_display_scale_surface(&state,&surface,&expanded)==ESP_OK);
    assert(expanded.native_width==width && expanded.native_height==height && expanded.stride==width);
    for (size_t i=0;i<(size_t)width*height;i++) assert(expanded.data[i]==17);
    for (unsigned rotation=0; rotation<4; rotation++) {
        unsigned nw=width/2, nh=height/2;
        surface.rotation=(solar_os_display_rotation_t)rotation;
        surface.width=rotation&1U ? nh:nw;
        surface.height=rotation&1U ? nw:nh;
        surface.stride=surface.width;
        for (unsigned y=0;y<surface.height;y++) for(unsigned x=0;x<surface.width;x++)
            pixels[(size_t)y*surface.stride+x]=(uint8_t)((x+7*y)%251);
        assert(solar_os_display_scale_surface(&state,&surface,&expanded)==ESP_OK);
        assert(expanded.rotation==SOLAR_OS_DISPLAY_ROTATION_0);
        for (unsigned y=0;y<height;y++) for(unsigned x=0;x<width;x++) {
            unsigned sx=x/2, sy=y/2;
            if(sx>=nw)sx=nw-1;
            if(sy>=nh)sy=nh-1;
            unsigned lx=sx,ly=sy;
            if(rotation==1){lx=sy;ly=nw-1-sx;}
            if(rotation==2){lx=nw-1-sx;ly=nh-1-sy;}
            if(rotation==3){lx=nh-1-sy;ly=sx;}
            assert(expanded.data[(size_t)y*width+x]==(uint8_t)((lx+7*ly)%251));
        }
        u8g2_SetDisplayRotation(&canvas,rotations[rotation]);
        solar_os_display_raster_t raster={.x=3,.y=4,.width=5,.height=6,.source_width=5,.source_height=6}, enlarged;
        assert(solar_os_display_scale_raster(&state,&raster,&enlarged)==ESP_OK);
        assert(enlarged.width==10 && enlarged.height==12);
        assert(enlarged.x==6+((rotation==2)?width%2:(rotation==3)?height%2:0));
        assert(enlarged.y==8+((rotation==1)?width%2:(rotation==2)?height%2:0));
    }
    for (unsigned format=0;format<=3;format++) {
        uint8_t image[64]={0};
        unsigned stride=format==SOLAR_OS_DISPLAY_FORMAT_MONO1 ? 1 :
            format==SOLAR_OS_DISPLAY_FORMAT_INDEX2 ? 2 :
            format==SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? 5 : 10;
        for (unsigned y=0;y<3;y++) for(unsigned x=0;x<5;x++) {
            uint8_t *row=image+y*stride;
            if(format==SOLAR_OS_DISPLAY_FORMAT_MONO1)row[0]|=((x+y)&1U)<<x;
            else if(format==SOLAR_OS_DISPLAY_FORMAT_RGB565){row[x*2]=0x10+y;row[x*2+1]=x+5*y;}
            else solar_os_indexed_set(row,x,(x+5*y)%(format==SOLAR_OS_DISPLAY_FORMAT_INDEX2?4:256),format);
        }
        solar_os_display_raster_t raster={.data=image,.data_size=sizeof(image),
            .source_width=5,.source_height=3,.source_stride=stride,.width=3,.height=2,.format=format}, enlarged;
        if(format==0){
            fail_allocation=true;
            assert(solar_os_display_scale_raster(&state,&raster,&enlarged)==ESP_ERR_NO_MEM);
            fail_allocation=false;
        }
        assert(solar_os_display_scale_raster(&state,&raster,&enlarged)==ESP_OK);
        assert(enlarged.source_width==3 && enlarged.source_height==2 && enlarged.width==6 && enlarged.height==4);
        for(unsigned y=0;y<2;y++)for(unsigned x=0;x<3;x++){
            unsigned sx=x*5/3,sy=y*3/2;
            const uint8_t *row=enlarged.data+y*enlarged.source_stride;
            if(format==SOLAR_OS_DISPLAY_FORMAT_MONO1)assert(((row[0]>>x)&1U)==((sx+sy)&1U));
            else if(format==SOLAR_OS_DISPLAY_FORMAT_RGB565)assert(row[x*2]==0x10+sy && row[x*2+1]==sx+5*sy);
            else assert(solar_os_indexed_pixel(row,x,format)==(sx+5*sy)%(format==SOLAR_OS_DISPLAY_FORMAT_INDEX2?4:256));
        }
    }
    solar_os_memory_free(state.raster_buffer);
    state.raster_buffer=NULL;state.raster_capacity=0;
    for (unsigned factor=1;factor<=4;factor++) {
        assert(solar_os_display_scale_prepare(&state,factor)==ESP_OK);
        solar_os_display_scale_apply(&state,factor);
        for(unsigned rotation=0;rotation<4;rotation++) {
            u8g2_SetDisplayRotation(&canvas,rotations[rotation]);
            uint8_t bitmap[]={0x53,0xff,0x2a,0xff,0x71,0xff};
            solar_os_display_raster_t raster={.data=bitmap,.data_size=sizeof(bitmap),
                .source_width=7,.source_height=3,.source_stride=2,.x=1,.y=2,
                .width=7,.height=3,.format=SOLAR_OS_DISPLAY_FORMAT_MONO1}, enlarged;
            if (factor > 1U) {
                fail_allocation=true;
                assert(solar_os_display_scale_bitmap(&state,&raster,&enlarged)==ESP_ERR_NO_MEM);
                fail_allocation=false;
            }
            assert(solar_os_display_scale_bitmap(&state,&raster,&enlarged)==ESP_OK);
            assert(enlarged.source_width==7*factor && enlarged.source_height==3*factor);
            for(unsigned y=0;y<enlarged.height;y++)for(unsigned x=0;x<enlarged.width;x++)
                assert(((enlarged.data[y*enlarged.source_stride+x/8]>>(x%8))&1U)==
                    ((bitmap[(y/factor)*2]>>(x/factor))&1U));
            raster.width=5; raster.height=7;
            assert(solar_os_display_scale_bitmap(&state,&raster,&enlarged)==ESP_OK);
            assert(enlarged.source_width==5*factor && enlarged.source_height==7*factor);
            for(unsigned y=0;y<enlarged.height;y++)for(unsigned x=0;x<enlarged.width;x++) {
                unsigned sx=x*7U/enlarged.width,sy=y*3U/enlarged.height;
                assert(((enlarged.data[y*enlarged.source_stride+x/8]>>(x%8))&1U)==
                    ((bitmap[sy*2]>>sx)&1U));
            }
            raster.data_size=1;
            assert(solar_os_display_scale_bitmap(&state,&raster,&enlarged)==ESP_ERR_INVALID_ARG);
            solar_os_memory_free(state.raster_buffer);
            state.raster_buffer=NULL;state.raster_capacity=0;
        }
    }
    free(pixels);
    solar_os_display_scale_apply(&state,1);
    assert(state.logical_buffer==NULL && state.surface_buffer==NULL && state.raster_buffer==NULL);
    solar_os_display_scale_destroy(&state);
    assert(canvas.u8x8.display_cb==driver && canvas.u8x8.display_info==&info && u8g2_GetBufferPtr(&canvas)==physical);
    free(physical);
}

int main(void)
{
    exercise(800,480); exercise(319,241); exercise(128,64);
    assert(tiles>0); puts("display scale: geometry, pixels, rotations, surfaces and cleanup passed");
    return 0;
}
