#pragma once
#include "esp_lcd_panel_rgb.h"
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t,int,int,int,int,const void *);
