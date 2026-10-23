#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *esp_lcd_panel_handle_t;
#define LCD_CLK_SRC_DEFAULT 0
typedef struct {
    uint32_t pclk_hz,h_res,v_res,hsync_pulse_width,hsync_back_porch,hsync_front_porch;
    uint32_t vsync_pulse_width,vsync_back_porch,vsync_front_porch;
    struct { unsigned hsync_idle_low:1,vsync_idle_low:1,de_idle_high:1,pclk_active_neg:1,pclk_idle_high:1; } flags;
} esp_lcd_rgb_timing_t;
typedef struct {
    int clk_src;
    esp_lcd_rgb_timing_t timings;
    size_t data_width,bits_per_pixel,num_fbs,dma_burst_size,bounce_buffer_size_px;
    int disp_gpio_num,hsync_gpio_num,vsync_gpio_num,de_gpio_num,pclk_gpio_num,data_gpio_nums[16];
    struct { unsigned fb_in_psram:1,no_fb:1; } flags;
} esp_lcd_rgb_panel_config_t;
typedef struct {
    bool (*on_bounce_empty)(esp_lcd_panel_handle_t,void *,int,int,void *);
} esp_lcd_rgb_panel_event_callbacks_t;
esp_err_t esp_lcd_rgb_panel_register_event_callbacks(esp_lcd_panel_handle_t,
    const esp_lcd_rgb_panel_event_callbacks_t *,void *);
esp_err_t esp_lcd_new_rgb_panel(const esp_lcd_rgb_panel_config_t *,esp_lcd_panel_handle_t *);
esp_err_t esp_lcd_rgb_panel_get_frame_buffer(esp_lcd_panel_handle_t,uint32_t,void **,...);
