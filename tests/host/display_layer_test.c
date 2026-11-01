#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_display_layer.h"
#include "solar_os_memory.h"

const u8g2_cb_t u8g2_cb_r0={0},u8g2_cb_r1={0},u8g2_cb_r2={0},u8g2_cb_r3={0};
void u8g2_ll_hvline_vertical_top_lsb(u8g2_t *c,u8g2_uint_t x,u8g2_uint_t y,u8g2_uint_t len,uint8_t dir)
{ (void)c;(void)x;(void)y;(void)len;(void)dir; }
void *solar_os_memory_alloc(size_t size,solar_os_memory_class_t policy,const char *tag)
{ (void)policy;(void)tag;return malloc(size); }
void *solar_os_memory_calloc(size_t n,size_t size,solar_os_memory_class_t policy,const char *tag)
{ (void)policy;(void)tag;return calloc(n,size); }
void solar_os_memory_free(void *p) {free(p);}
static uint16_t pixel(const solar_os_display_raster_t *r,unsigned x,unsigned y)
{ const uint8_t *p=r->data+y*r->source_stride+x*2U;return (p[0]<<8)|p[1]; }
int main(void)
{
    const u8g2_cb_t *rotations[]={U8G2_R0,U8G2_R1,U8G2_R2,U8G2_R3};
    for (unsigned r=0;r<4;r++) {
        const u8x8_display_info_t info={.pixel_width=32,.pixel_height=24,.tile_width=4,.tile_height=3};
        uint8_t original[96];memset(original,0xff,sizeof(original));
        u8g2_t canvas={.width=r%2?24:32,.height=r%2?32:24,.tile_buf_height=3,
            .tile_buf_ptr=original,.cb=rotations[r],.ll_hvline=u8g2_ll_hvline_vertical_top_lsb,
            .u8x8={.display_info=&info}};
        solar_os_display_layer_t *l=solar_os_display_layer_create(&canvas,false,true);
        assert(l && canvas.tile_buf_ptr!=original);
        uint8_t ink[4]={0x3f,0x01,0x20,0x3f};
        solar_os_display_layer_update(l,ink,5,8,6,4);
        uint8_t data[32*32*2];
        for (unsigned i=0;i<sizeof(data);i+=2) {data[i]=0;data[i+1]=0x1f;}
        solar_os_display_raster_t input={.data=data,.data_size=sizeof(data),.source_width=canvas.width,
            .source_height=canvas.height,.source_stride=canvas.width*2,.width=canvas.width,
            .height=canvas.height,.format=SOLAR_OS_DISPLAY_FORMAT_RGB565};
        solar_os_display_raster_t out;
        solar_os_display_layer_frame(l,&input,&out);
        assert(pixel(&out,0,0)==0x001f && pixel(&out,5,8)==0 && pixel(&out,6,9)==0xffff);
        /* Rendering an updated overlay must preserve the RGB backing plane. */
        (void)solar_os_display_layer_canvas(l,false);
        solar_os_display_layer_raster(l,&out);assert(pixel(&out,0,0)==0x001f);
        uint8_t packed[4];canvas.tile_buf_ptr=solar_os_display_layer_canvas(l,false);
        solar_os_display_canvas_pack(&canvas,packed,5,8,6,4);
        /* Canvas polarity differs from the normalized layer; pack with black=1
         * is for the separate keyboard drawing canvas, not the target canvas. */
        assert((packed[0]&0x3f)==0);
        canvas.tile_buf_ptr=solar_os_display_layer_base(l);
        uint8_t x,y,w,h;solar_os_display_layer_area(l,&x,&y,&w,&h);
        assert(w && h && x+w<=4 && y+h<=3);
        uint16_t palette[256]={0};palette[1]=0x07e0;
        uint8_t indexed[32*32];memset(indexed,1,sizeof(indexed));
        solar_os_display_surface_t surface={.data=indexed,.data_size=sizeof(indexed),
            .width=canvas.width,.height=canvas.height,.stride=canvas.width,.palette_rgb565=palette,
            .palette_size=256,.rotation=r,.native_width=32,.native_height=24};
        solar_os_display_layer_surface(l,&surface,&out);
        assert(pixel(&out,0,0)==0x07e0 && pixel(&out,5,8)==0);
        /* Move the layer: the old rectangle reveals the current app image. */
        solar_os_display_layer_update(l,ink,12,16,6,4);
        solar_os_display_layer_raster(l,&out);assert(pixel(&out,5,8)==0x07e0);
        solar_os_display_layer_destroy(l);assert(canvas.tile_buf_ptr==original);
    }
    for (unsigned polarity=0;polarity<2;polarity++) {
        const u8x8_display_info_t info={.pixel_width=32,.pixel_height=24,.tile_width=4,.tile_height=3};
        uint8_t original[96];memset(original,polarity?0:255,sizeof(original));
        u8g2_t canvas={.width=32,.height=24,.tile_buf_height=3,.tile_buf_ptr=original,.cb=U8G2_R0,
            .ll_hvline=u8g2_ll_hvline_vertical_top_lsb,.u8x8={.display_info=&info}};
        solar_os_display_layer_t *l=solar_os_display_layer_create(&canvas,polarity,false);
        uint8_t ink=1;
        solar_os_display_layer_update(l,&ink,8,8,1,1);
        uint8_t data=1;
        solar_os_display_raster_t input={.data=&data,.data_size=1,.source_width=1,.source_height=1,
            .source_stride=1,.x=2,.y=3,.width=1,.height=1,.format=SOLAR_OS_DISPLAY_FORMAT_MONO1,
            .clear_background=true,.background_index=0};
        solar_os_display_raster_t out;
        solar_os_display_layer_frame(l,&input,&out);
        assert(out.format==SOLAR_OS_DISPLAY_FORMAT_MONO1 && out.data[3*4]==4 && out.data[8*4+1]==1);
        assert(out.data[0]==0);
        solar_os_display_layer_uncover(l);solar_os_display_layer_raster(l,&out);
        assert(out.data[3*4]==4 && out.data[8*4+1]==0);
        solar_os_display_layer_destroy(l);
        assert(((original[2]>>3)&1)==polarity);
    }
    puts("display layer composition tests: ok");
}
