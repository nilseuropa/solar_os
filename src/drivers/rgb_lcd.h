#pragma once
#include "esp_lcd_panel_rgb.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_display_surface.h"
#include "u8g2.h"

typedef struct {
    esp_lcd_rgb_panel_config_t panel;
    bool have_reset, have_power, have_backlight;
    solar_os_gpio_line_ref_t reset, power, backlight;
} solar_os_rgb_lcd_config_t;

typedef struct {
    solar_os_rgb_lcd_config_t config;
    esp_lcd_panel_handle_t panel;
    uint16_t *pixels;
    uint8_t *mono;
    u8g2_t u8g2;
    u8x8_display_info_t info;
    uint16_t foreground, background;
    uint8_t brightness;
    bool sleeping;
    esp_err_t last_error;
} solar_os_rgb_lcd_t;

esp_err_t solar_os_rgb_lcd_init(solar_os_rgb_lcd_t *, const solar_os_rgb_lcd_config_t *);
esp_err_t solar_os_rgb_lcd_resume(solar_os_rgb_lcd_t *);
esp_err_t solar_os_rgb_lcd_shutdown(solar_os_rgb_lcd_t *);
void solar_os_rgb_lcd_deinit(solar_os_rgb_lcd_t *);
esp_err_t solar_os_rgb_lcd_brightness(solar_os_rgb_lcd_t *, uint8_t);
esp_err_t solar_os_rgb_lcd_power_save(solar_os_rgb_lcd_t *, bool);
esp_err_t solar_os_rgb_lcd_colors(solar_os_rgb_lcd_t *, uint32_t, uint32_t);
esp_err_t solar_os_rgb_lcd_frame(solar_os_rgb_lcd_t *, const solar_os_display_raster_t *);
esp_err_t solar_os_rgb_lcd_surface(solar_os_rgb_lcd_t *, const solar_os_display_surface_t *);
