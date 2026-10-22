#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rgb_lcd_raster.h"

int main(void)
{
    uint16_t storage[18], *out=storage+1;
    for (unsigned i=0;i<18;i++) storage[i]=0xaaaa;
    const uint8_t rgb[]={0xf8,0x00,0x07,0xe0, 0x00,0x1f,0xff,0xff};
    solar_os_display_raster_t f={.data=rgb,.data_size=sizeof(rgb),.source_width=2,
        .source_height=2,.source_stride=4,.width=4,.height=4,.format=SOLAR_OS_DISPLAY_FORMAT_RGB565};
    assert(solar_os_rgb_lcd_render(out,4,4,&f,0,0xffff)==ESP_OK);
    const uint16_t expected[]={0xf800,0xf800,0x07e0,0x07e0, 0xf800,0xf800,0x07e0,0x07e0,
        0x001f,0x001f,0xffff,0xffff, 0x001f,0x001f,0xffff,0xffff};
    assert(!memcmp(out,expected,sizeof(expected)));
    assert(storage[0]==0xaaaa && storage[17]==0xaaaa);
    f.data_size=7;
    assert(solar_os_rgb_lcd_render(out,4,4,&f,0,0xffff)==ESP_ERR_INVALID_SIZE);
    assert(!memcmp(out,expected,sizeof(expected)));
    f.data_size=8; f.x=1;
    assert(solar_os_rgb_lcd_render(out,4,4,&f,0,0xffff)==ESP_ERR_INVALID_ARG);
    const uint8_t indexed[]={0xe4}; /* 0,1,2,3 in low-bit-first order. */
    const uint16_t colors[]={0xffff,0xaaaa,0x5555,0};
    f=(solar_os_display_raster_t){.data=indexed,.data_size=1,.source_width=4,.source_height=1,
        .source_stride=1,.width=4,.height=1,.palette_rgb565=colors,.palette_size=4,
        .format=SOLAR_OS_DISPLAY_FORMAT_INDEX2,.palette_inverted=true};
    assert(solar_os_rgb_lcd_render(out,4,4,&f,0,0xffff)==ESP_OK);
    assert(out[0]==0 && out[1]==0x5555 && out[2]==0xaaaa && out[3]==0xffff);
    const uint16_t oversized_palette[]={0,1,2,3,4,5,6,7};
    f.palette_rgb565=oversized_palette; f.palette_size=8; f.clear_background=true; f.background_index=7;
    assert(solar_os_rgb_lcd_render(out,4,4,&f,0,0xffff)==ESP_ERR_INVALID_ARG);
    f.palette_rgb565=colors; f.palette_size=4; f.clear_background=false; f.background_index=0;
    uint8_t index8[]={3,0,1,2};
    f.data=index8; f.data_size=4; f.source_stride=4; f.format=SOLAR_OS_DISPLAY_FORMAT_INDEX8;
    assert(solar_os_rgb_lcd_render(out,4,4,&f,0,0xffff)==ESP_OK && out[0]==0xffff);
    uint16_t before[16]; memcpy(before,out,sizeof(before));
    index8[3]=4;
    assert(solar_os_rgb_lcd_render(out,4,4,&f,0,0xffff)==ESP_ERR_INVALID_ARG);
    assert(!memcmp(out,before,sizeof(before)));
    const uint8_t mono[]={5};
    f=(solar_os_display_raster_t){.data=mono,.data_size=1,.source_width=3,.source_height=1,
        .source_stride=1,.width=3,.height=1,.format=SOLAR_OS_DISPLAY_FORMAT_MONO1,
        .clear_background=true};
    assert(solar_os_rgb_lcd_render(out,4,4,&f,0x1234,0xffff)==ESP_OK);
    assert(out[0]==0x1234 && out[1]==0xffff && out[2]==0x1234 && out[15]==0xffff);
    const uint8_t pixels[]={0,1,0,2, 0,3,0,4, 0,5,0,6};
    solar_os_display_surface_t s={.data=pixels,.data_size=12,.width=2,.height=3,.stride=4,
        .native_width=3,.native_height=2,.format=SOLAR_OS_DISPLAY_FORMAT_RGB565,
        .rotation=SOLAR_OS_DISPLAY_ROTATION_90};
    const uint16_t rotated90[]={5,3,1,6,4,2}, rotated270[]={2,4,6,1,3,5};
    assert(solar_os_rgb_lcd_render_surface(out,3,2,&s,0,0xffff)==ESP_OK);
    assert(!memcmp(out,rotated90,sizeof(rotated90)));
    s.rotation=SOLAR_OS_DISPLAY_ROTATION_270;
    assert(solar_os_rgb_lcd_render_surface(out,3,2,&s,0,0xffff)==ESP_OK);
    assert(!memcmp(out,rotated270,sizeof(rotated270)));
    s.native_width=2;
    assert(solar_os_rgb_lcd_render_surface(out,3,2,&s,0,0xffff)==ESP_ERR_INVALID_ARG);
    puts("RGB LCD: RGB565 byte order, scaled/indexed/mono pixels, rotations and preflight bounds: ok");
}
