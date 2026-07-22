#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *panel;
    u8g2_t u8g2;
    uint8_t *buffer;
    /* Double-buffered scan-out: tiles render into framebuffers[back_fb]
     * while the panel DMA reads the other one; the buffers swap at the
     * next frame boundary once a full frame has been written. */
    void *framebuffers[2];
    uint8_t back_fb;
    /* Copy of the native 1bpp buffer as of the last present, plus a
     * bitmap of native columns the back framebuffer is still missing:
     * together they let a present convert/write only the physical rows
     * that actually changed. */
    uint8_t *shadow;
    uint8_t dirty_prev[64];
    size_t buffer_size;
    esp_err_t last_error;
    uint8_t backlight_percent;
    bool backlight_power;
} lcd_rgb_panel_t;

esp_err_t lcd_rgb_panel_init(lcd_rgb_panel_t *display);
esp_err_t lcd_rgb_panel_resume(lcd_rgb_panel_t *display);
void lcd_rgb_panel_deinit(lcd_rgb_panel_t *display);
u8g2_t *lcd_rgb_panel_get_u8g2(lcd_rgb_panel_t *display);
bool lcd_rgb_panel_backlight_supported(void);
esp_err_t lcd_rgb_panel_get_backlight(const lcd_rgb_panel_t *display, uint8_t *percent);
esp_err_t lcd_rgb_panel_set_backlight(lcd_rgb_panel_t *display, uint8_t percent);

#ifdef __cplusplus
}
#endif
