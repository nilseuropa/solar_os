#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/drivers/rgb_lcd.c"

static uint16_t scanout[16*16];
static unsigned allocated, deleted;
static bool fail_init, fail_create, fail_callbacks, bounce_mode;
static esp_lcd_rgb_panel_event_callbacks_t callbacks;
static void *callback_context;
static bool light;
esp_err_t esp_lcd_new_rgb_panel(const esp_lcd_rgb_panel_config_t *c,esp_lcd_panel_handle_t *p)
{
    assert(c->data_width==16);
    bounce_mode=c->bounce_buffer_size_px!=0;
    if (bounce_mode) assert(c->flags.no_fb && !c->num_fbs && !c->flags.fb_in_psram);
    if (fail_create) return ESP_ERR_NO_MEM;
    *p=scanout; allocated++; return ESP_OK;
}
esp_err_t esp_lcd_rgb_panel_register_event_callbacks(esp_lcd_panel_handle_t p,
    const esp_lcd_rgb_panel_event_callbacks_t *c,void *ctx)
{ assert(p==scanout && bounce_mode); callbacks=*c; callback_context=ctx; return fail_callbacks?ESP_FAIL:ESP_OK; }
esp_err_t esp_lcd_rgb_panel_get_frame_buffer(esp_lcd_panel_handle_t p,uint32_t n,void **fb,...)
{ assert(p==scanout && n==1 && !bounce_mode); *fb=scanout; return ESP_OK; }
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t p) { assert(p); return ESP_OK; }
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t p) { assert(p); return fail_init?ESP_FAIL:ESP_OK; }
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t p) { assert(p==scanout); deleted++; return ESP_OK; }
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p,int x,int y,int w,int h,const void *f)
{ assert(p && !bounce_mode && !x && !y && w==16 && h==16 && f==scanout); return ESP_OK; }
esp_err_t solar_os_gpio_line_write(const solar_os_gpio_line_ref_t *l,bool value)
{ assert(l->line==2); light=value; return ESP_OK; }
void vTaskDelay(TickType_t ticks) { assert(ticks); }

static void check_full_size_rendering(solar_os_rgb_lcd_t *d)
{
    const unsigned width=1024, height=600;
    /* Each tile row differs, so a send offset wrapping at 64 KiB cannot
     * silently repeat the status bar and shell in the bottom 88 rows. */
    for(unsigned row=0;row<height/8;row++)
        memset(d->mono+row*width,(int)(row+1),width);
    u8g2_SendBuffer(&d->u8g2);
    for(unsigned y=0;y<height;y++)
        for(unsigned x=0;x<width;x++)
            assert(d->pixels[y*width+x]==
                (((y/8+1)&(1U<<(y%8))) ? 0 : 0xffff));

    /* Exercise drawing offsets independently of sending. Include both sides
     * of row 512 and a vertical line crossing that boundary. */
    u8g2_ClearBuffer(&d->u8g2);
    u8g2_DrawHLine(&d->u8g2,0,0,width);
    u8g2_DrawHLine(&d->u8g2,0,511,width);
    u8g2_DrawHLine(&d->u8g2,0,512,width);
    u8g2_DrawHLine(&d->u8g2,0,599,width);
    u8g2_DrawVLine(&d->u8g2,1023,500,100);
    for(unsigned y=0;y<height;y++)
        for(unsigned x=0;x<width;x++) {
            const bool set=y==0 || y==511 || y==512 || y==599 || (x==1023 && y>=500);
            assert(!!(d->mono[(y/8)*width+x]&(1U<<(y%8)))==set);
        }
    u8g2_SendBuffer(&d->u8g2);
    for(unsigned y=0;y<height;y++)
        for(unsigned x=0;x<width;x++) {
            const bool set=y==0 || y==511 || y==512 || y==599 || (x==1023 && y>=500);
            assert(d->pixels[y*width+x]==(set ? 0 : 0xffff));
        }
    u8g2_ClearBuffer(&d->u8g2);
    u8g2_SendBuffer(&d->u8g2);
    for(unsigned i=0;i<width*height;i++) assert(d->pixels[i]==0xffff);

    /* The alternate U8g2 byte layout must address the same full buffer. */
    u8g2_SetupBuffer(&d->u8g2,d->mono,height/8,
        u8g2_ll_hvline_horizontal_right_lsb,U8G2_R0);
    u8g2_DrawHLine(&d->u8g2,0,512,width);
    u8g2_DrawVLine(&d->u8g2,1023,500,100);
    for(unsigned y=0;y<height;y++)
        for(unsigned x=0;x<width;x++) {
            const bool set=y==512 || (x==1023 && y>=500);
            assert(!!(d->mono[y*(width/8)+x/8]&(128U>>(x%8)))==set);
        }
    u8g2_SetupBuffer(&d->u8g2,d->mono,height/8,
        u8g2_ll_hvline_vertical_top_lsb,U8G2_R0);
}

int main(void)
{
    solar_os_rgb_lcd_config_t c={.panel={.timings={.h_res=16,.v_res=16,.pclk_hz=1000000},
        .data_width=16,.bits_per_pixel=16,.num_fbs=1,.hsync_gpio_num=16,.vsync_gpio_num=17,
        .de_gpio_num=18,.pclk_gpio_num=19},.have_backlight=true,.backlight={.controller="io0",.line=2}};
    for(unsigned i=0;i<16;i++) c.panel.data_gpio_nums[i]=i;
    solar_os_rgb_lcd_t d;
    fail_init=true;
    assert(solar_os_rgb_lcd_init(&d,&c)==ESP_FAIL && allocated==deleted && !active && !light);
    fail_init=false;
    assert(solar_os_rgb_lcd_init(&d,&c)==ESP_OK && active==&d && light);
    assert(u8g2_GetDisplayWidth(&d.u8g2)==16 && u8g2_GetDisplayHeight(&d.u8g2)==16);
    /* Clear/fill repeat ONE tile descriptor, not an array of descriptors. */
    u8x8_FillDisplay(u8g2_GetU8x8(&d.u8g2));
    for(unsigned i=0;i<256;i++) assert(scanout[i]==0);
    u8x8_ClearDisplay(u8g2_GetU8x8(&d.u8g2));
    for(unsigned i=0;i<256;i++) assert(scanout[i]==0xffff);
    assert(solar_os_rgb_lcd_brightness(&d,0)==ESP_OK && !light);
    assert(solar_os_rgb_lcd_shutdown(&d)==ESP_OK && !d.panel && !light);
    assert(solar_os_rgb_lcd_resume(&d)==ESP_OK && d.panel && !light);
    assert(solar_os_rgb_lcd_brightness(&d,50)==ESP_OK && light);
    assert(solar_os_rgb_lcd_power_save(&d,true)==ESP_OK && !light);
    assert(solar_os_rgb_lcd_power_save(&d,false)==ESP_OK && light);
    solar_os_rgb_lcd_deinit(&d);
    assert(allocated==deleted && !active && !light);
    /* Production bounce scanout owns CPU-only PSRAM; IDF owns DMA buffers.
     * Exercise allocation/registration/init rollback and repeated wake cycles. */
    c.panel.bounce_buffer_size_px=16*8;
    c.panel.flags.fb_in_psram=1;
    fail_create=true;
    assert(solar_os_rgb_lcd_init(&d,&c)==ESP_ERR_NO_MEM && !d.pixels && !d.mono && !light);
    fail_create=false; fail_callbacks=true;
    assert(solar_os_rgb_lcd_init(&d,&c)==ESP_FAIL && allocated==deleted && !d.pixels && !light);
    fail_callbacks=false; fail_init=true;
    assert(solar_os_rgb_lcd_init(&d,&c)==ESP_FAIL && allocated==deleted && !d.pixels && !light);
    fail_init=false;
    assert(solar_os_rgb_lcd_init(&d,&c)==ESP_OK && light && d.pixels!=scanout);
    uint16_t bounce[128];
    for(unsigned cycle=0;cycle<3;cycle++) {
        for(unsigned i=0;i<256;i++) d.pixels[i]=(uint16_t)(i+cycle);
        for(unsigned pos=0;pos<256;pos+=128) {
            assert(!callbacks.on_bounce_empty(d.panel,bounce,pos,sizeof(bounce),callback_context));
            for(unsigned i=0;i<128;i++) assert(bounce[i]==pos+i+cycle);
        }
        u8x8_ClearDisplay(u8g2_GetU8x8(&d.u8g2));
        for(unsigned i=0;i<256;i++) assert(d.pixels[i]==0xffff);
        assert(solar_os_rgb_lcd_shutdown(&d)==ESP_OK && !d.pixels && !light);
        assert(solar_os_rgb_lcd_resume(&d)==ESP_OK && d.pixels && light);
    }
    solar_os_rgb_lcd_deinit(&d);
    assert(allocated==deleted && !active && !light);
    c.panel.bounce_buffer_size_px=17;
    assert(solar_os_rgb_lcd_init(&d,&c)==ESP_ERR_INVALID_ARG);
    c.panel.bounce_buffer_size_px=128;
    c.panel.data_gpio_nums[0]=19;
    assert(solar_os_rgb_lcd_init(&d,&c)==ESP_ERR_INVALID_ARG);
    c.panel.data_gpio_nums[0]=0;
    c.panel.timings.h_res=1024; c.panel.timings.v_res=600;
    c.panel.bounce_buffer_size_px=1024*10;
    assert(solar_os_rgb_lcd_init(&d,&c)==ESP_OK);
    assert(u8g2_GetDisplayWidth(&d.u8g2)==1024 && u8g2_GetDisplayHeight(&d.u8g2)==600);
    uint16_t full_bounce[1024*10];
    d.pixels[1024*600-1]=0x1234;
    assert(!callbacks.on_bounce_empty(d.panel,full_bounce,1024*590,sizeof(full_bounce),callback_context));
    assert(full_bounce[0]==0xffff && full_bounce[1024*10-1]==0x1234);
    check_full_size_rendering(&d);
    solar_os_rgb_lcd_deinit(&d);
    assert(allocated==deleted && !active && !light);
    puts("RGB LCD: direct/bounce scanout, 1024x600 pixels, rollback, wake and brightness: ok");
}
