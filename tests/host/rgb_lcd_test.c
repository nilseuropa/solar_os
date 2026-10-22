#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/drivers/rgb_lcd.c"

static uint16_t scanout[16*16];
static unsigned allocated, deleted;
static bool fail_init;
static bool light;
esp_err_t esp_lcd_new_rgb_panel(const esp_lcd_rgb_panel_config_t *c,esp_lcd_panel_handle_t *p)
{ assert(c->data_width==16); *p=scanout; allocated++; return ESP_OK; }
esp_err_t esp_lcd_rgb_panel_get_frame_buffer(esp_lcd_panel_handle_t p,uint32_t n,void **fb,...)
{ assert(p==scanout && n==1); *fb=scanout; return ESP_OK; }
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t p) { assert(p); return ESP_OK; }
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t p) { assert(p); return fail_init?ESP_FAIL:ESP_OK; }
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t p) { assert(p==scanout); deleted++; return ESP_OK; }
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p,int x,int y,int w,int h,const void *f)
{ assert(p && !x && !y && w==16 && h==16 && f==scanout); return ESP_OK; }
esp_err_t solar_os_gpio_line_write(const solar_os_gpio_line_ref_t *l,bool value)
{ assert(l->line==2); light=value; return ESP_OK; }
void vTaskDelay(TickType_t ticks) { assert(ticks); }

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
    c.panel.data_gpio_nums[0]=19;
    assert(solar_os_rgb_lcd_init(&d,&c)==ESP_ERR_INVALID_ARG);
    puts("RGB LCD lifecycle, U8g2 repeated tiles, failed init and brightness restoration: ok");
}
