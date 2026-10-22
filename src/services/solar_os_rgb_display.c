#include "solar_os_rgb_display.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "rgb_lcd.h"
#include "solar_os_board_display.h"
#include "solar_os_display.h"

#define RGB_FORMATS (SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT | SOLAR_OS_DISPLAY_FORMAT_INDEX2_BIT | SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT | SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT)

typedef struct {
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    bool primary;
    solar_os_rgb_lcd_t driver;
    solar_os_board_display_t display;
} rgb_device_t;
static rgb_device_t *device;

static esp_err_t runtime_ready(solar_os_board_display_t *d)
{ return d && d->driver && ((solar_os_rgb_lcd_t *)d->driver)->panel ? ESP_OK : ESP_ERR_INVALID_STATE; }
static esp_err_t resume(solar_os_board_display_t *d)
{
    esp_err_t err = solar_os_rgb_lcd_resume(d->driver);
    d->ready = err == ESP_OK;
    return err;
}
static esp_err_t shutdown(solar_os_board_display_t *d)
{
    esp_err_t err = solar_os_rgb_lcd_shutdown(d->driver);
    if (err == ESP_OK) d->ready = false;
    return err;
}
static void deinit(solar_os_board_display_t *d)
{ solar_os_rgb_lcd_deinit(d->driver); d->ready = false; }
static bool brightness_supported(const solar_os_board_display_t *d)
{ return d && ((solar_os_rgb_lcd_t *)d->driver)->config.have_backlight; }
static esp_err_t get_brightness(const solar_os_board_display_t *d, uint8_t *percent)
{
    if (!d || !percent) return ESP_ERR_INVALID_ARG;
    *percent = ((solar_os_rgb_lcd_t *)d->driver)->brightness;
    return ESP_OK;
}
static esp_err_t set_brightness(solar_os_board_display_t *d, uint8_t percent)
{ return solar_os_rgb_lcd_brightness(d->driver, percent); }
static esp_err_t set_colors(solar_os_board_display_t *d, uint32_t fg, uint32_t bg)
{ return solar_os_rgb_lcd_colors(d->driver, fg, bg); }
static esp_err_t power_save(solar_os_board_display_t *d, bool enabled)
{ return solar_os_rgb_lcd_power_save(d->driver, enabled); }
static esp_err_t present_frame(solar_os_board_display_t *d, const solar_os_display_raster_t *f)
{ return solar_os_rgb_lcd_frame(d->driver, f); }
static esp_err_t present_surface(solar_os_board_display_t *d, const solar_os_display_surface_t *s)
{ return solar_os_rgb_lcd_surface(d->driver, s); }
static esp_err_t auxiliary_frame(void *ctx, const solar_os_display_raster_t *f)
{ return solar_os_rgb_lcd_frame(ctx, f); }
static esp_err_t auxiliary_surface(void *ctx, const solar_os_display_surface_t *s)
{ return solar_os_rgb_lcd_surface(ctx, s); }

static const solar_os_board_display_ops_t ops = {
    .runtime_ready=runtime_ready, .resume=resume, .shutdown=shutdown, .deinit=deinit,
    .brightness_supported=brightness_supported, .get_brightness=get_brightness,
    .set_brightness=set_brightness, .set_colors=set_colors, .set_power_save=power_save,
    .present_frame=present_frame, .present_surface=present_surface,
};

static esp_err_t parse(const solar_os_expansion_binding_t *bindings, size_t count,
                         solar_os_rgb_lcd_config_t *c)
{
    *c = (solar_os_rgb_lcd_config_t){.panel={
        .clk_src=LCD_CLK_SRC_DEFAULT, .data_width=16, .bits_per_pixel=16,
        .num_fbs=1, .dma_burst_size=64, .disp_gpio_num=-1,
        .hsync_gpio_num=-1, .vsync_gpio_num=-1, .de_gpio_num=-1, .pclk_gpio_num=-1,
        .flags.fb_in_psram=1,
    }};
    for (unsigned i = 0; i < 16; i++) c->panel.data_gpio_nums[i] = -1;
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO) {
            if (b->role[0] == 'd' && b->role[1] >= '0' && b->role[1] <= '9') {
                char *end;
                long n = strtol(b->role + 1, &end, 10);
                if (*end || n >= 16) return ESP_ERR_INVALID_ARG;
                c->panel.data_gpio_nums[n] = b->value;
            } else if (!strcmp(b->role,"hsync")) c->panel.hsync_gpio_num=b->value;
            else if (!strcmp(b->role,"vsync")) c->panel.vsync_gpio_num=b->value;
            else if (!strcmp(b->role,"de")) c->panel.de_gpio_num=b->value;
            else if (!strcmp(b->role,"clk")) c->panel.pclk_gpio_num=b->value;
            else return ESP_ERR_INVALID_ARG;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO_LINE) {
            solar_os_gpio_line_ref_t *line;
            if (!strcmp(b->role,"reset")) { line=&c->reset; c->have_reset=true; }
            else if (!strcmp(b->role,"power")) { line=&c->power; c->have_power=true; }
            else if (!strcmp(b->role,"bl")) { line=&c->backlight; c->have_backlight=true; }
            else return ESP_ERR_INVALID_ARG;
            if (b->value < 0 || b->value > UINT8_MAX) return ESP_ERR_INVALID_ARG;
            strlcpy(line->controller,b->target,sizeof(line->controller)); line->line=b->value;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && b->value >= 0) {
            if (!strcmp(b->role,"width")) c->panel.timings.h_res=b->value;
            else if (!strcmp(b->role,"height")) c->panel.timings.v_res=b->value;
            else if (!strcmp(b->role,"clock")) c->panel.timings.pclk_hz=(uint32_t)b->value*1000U;
            else if (!strcmp(b->role,"hsw")) c->panel.timings.hsync_pulse_width=b->value;
            else if (!strcmp(b->role,"hbp")) c->panel.timings.hsync_back_porch=b->value;
            else if (!strcmp(b->role,"hfp")) c->panel.timings.hsync_front_porch=b->value;
            else if (!strcmp(b->role,"vsw")) c->panel.timings.vsync_pulse_width=b->value;
            else if (!strcmp(b->role,"vbp")) c->panel.timings.vsync_back_porch=b->value;
            else if (!strcmp(b->role,"vfp")) c->panel.timings.vsync_front_porch=b->value;
            else if (!strcmp(b->role,"flags")) {
                c->panel.timings.flags.hsync_idle_low=(b->value & 1U)!=0;
                c->panel.timings.flags.vsync_idle_low=(b->value & 2U)!=0;
                c->panel.timings.flags.de_idle_high=(b->value & 4U)!=0;
                c->panel.timings.flags.pclk_active_neg=(b->value & 8U)!=0;
                c->panel.timings.flags.pclk_idle_high=(b->value & 16U)!=0;
            } else return ESP_ERR_INVALID_ARG;
        } else return ESP_ERR_INVALID_ARG;
    }
    c->panel.bounce_buffer_size_px=c->panel.timings.h_res *
        (c->panel.timings.v_res % 10U == 0 ? 10U : 8U);
    return ESP_OK;
}

esp_err_t solar_os_rgb_display_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (!name || !name[0] || !bindings) return ESP_ERR_INVALID_ARG;
    if (device) return ESP_ERR_INVALID_STATE;
    solar_os_rgb_lcd_config_t config;
    esp_err_t err = parse(bindings,count,&config);
    if (err != ESP_OK) return err;
    rgb_device_t *d=heap_caps_calloc(1,sizeof(*d),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!d) return ESP_ERR_NO_MEM;
    err=solar_os_rgb_lcd_init(&d->driver,&config);
    if (err != ESP_OK) { heap_caps_free(d); return err; }
    strlcpy(d->name,name,sizeof(d->name));
    d->primary=solar_os_display_target_is_board_primary(name);
    d->display=(solar_os_board_display_t){
        .ops=&ops,.driver=&d->driver,.driver_name="rgb-lcd",.controller="RGB565",
        .u8g2=&d->driver.u8g2,.width=config.panel.timings.h_res,.height=config.panel.timings.v_res,
        .surface_formats=RGB_FORMATS,.frame_formats=RGB_FORMATS,
        .preferred_stream_fps=20,.max_stream_pixels_per_second=16000000U,.ready=true,
    };
    if (d->primary) err=solar_os_board_display_register_primary(&d->display);
    else {
        solar_os_display_target_t target={0};
        strlcpy(target.name,name,sizeof(target.name));
        strlcpy(target.source,"expansion",sizeof(target.source));
        strlcpy(target.driver,"rgb-lcd",sizeof(target.driver));
        strlcpy(target.controller,"RGB565",sizeof(target.controller));
        strlcpy(target.role,"aux",sizeof(target.role));
        target.width=d->display.width; target.height=d->display.height; target.ready=true;
        target.u8g2=d->display.u8g2; target.surface_formats=RGB_FORMATS; target.frame_formats=RGB_FORMATS;
        target.preferred_stream_fps=d->display.preferred_stream_fps;
        target.max_stream_pixels_per_second=d->display.max_stream_pixels_per_second;
        target.surface_context=&d->driver; target.present_surface=auxiliary_surface;
        target.frame_context=&d->driver; target.present_frame=auxiliary_frame;
        err=solar_os_display_register_target(&target);
    }
    if (err != ESP_OK) { solar_os_rgb_lcd_deinit(&d->driver); heap_caps_free(d); return err; }
    device=d;
    return ESP_OK;
}

esp_err_t solar_os_rgb_display_detach(const char *name)
{
    if (!device || !name || strcmp(name,device->name)) return ESP_ERR_NOT_FOUND;
    const esp_err_t err=device->primary ? solar_os_board_display_unregister_primary(&device->display) :
        solar_os_display_unregister_target(name);
    if (err != ESP_OK) return err;
    solar_os_rgb_lcd_deinit(&device->driver);
    heap_caps_free(device); device=NULL;
    return ESP_OK;
}
