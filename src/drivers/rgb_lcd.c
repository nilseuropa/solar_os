#include "rgb_lcd.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rgb_lcd_raster.h"

static solar_os_rgb_lcd_t *active;

static bool IRAM_ATTR fill_bounce(esp_lcd_panel_handle_t panel, void *buffer,
                                int pos_px, int len_bytes, void *context)
{
    (void)panel;
    const solar_os_rgb_lcd_t *d = context;
    memcpy(buffer, d->pixels + pos_px, len_bytes);
    return false;
}

static esp_err_t backlight(solar_os_rgb_lcd_t *d)
{
    return d->config.have_backlight ? solar_os_gpio_line_write(&d->config.backlight,
        !d->sleeping && d->brightness != 0) : ESP_OK;
}

static esp_err_t flush(solar_os_rgb_lcd_t *d)
{
    if (!d || !d->panel || !d->pixels) return ESP_ERR_INVALID_STATE;
    /* Bounce scanout continuously copies the CPU-visible pixels through cache.
     * Only the IDF-owned direct-DMA framebuffer needs draw/cache synchronization. */
    if (d->config.panel.bounce_buffer_size_px) return ESP_OK;
    return esp_lcd_panel_draw_bitmap(d->panel, 0, 0,
        d->config.panel.timings.h_res, d->config.panel.timings.v_res, d->pixels);
}

static esp_err_t start_panel(solar_os_rgb_lcd_t *d)
{
    esp_err_t err = ESP_OK;
    if (d->config.have_power) err = solar_os_gpio_line_write(&d->config.power, true);
    if (err != ESP_OK) return err;
    if (d->config.have_reset) {
        err = solar_os_gpio_line_write(&d->config.reset, false);
        if (err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(10) + 1U);
        err = solar_os_gpio_line_write(&d->config.reset, true);
        if (err != ESP_OK) return err;
    }
    vTaskDelay(pdMS_TO_TICKS(120) + 1U);
    esp_lcd_rgb_panel_config_t panel = d->config.panel;
    const size_t count = (size_t)panel.timings.h_res * panel.timings.v_res;
    if (panel.bounce_buffer_size_px) {
        /* CPU-only pixels belong in managed PSRAM. IDF 5.5.5 requests
         * SPIRAM|DMA for its framebuffer, which S3's PSRAM heap cannot supply.
         * no_fb lets IDF own only the internal DMA bounce buffers. */
        d->pixels = heap_caps_calloc(count, sizeof(*d->pixels), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!d->pixels) err = ESP_ERR_NO_MEM;
        panel.num_fbs = 0;
        panel.flags.no_fb = 1;
        panel.flags.fb_in_psram = 0;
    }
    if (err == ESP_OK) err = esp_lcd_new_rgb_panel(&panel, &d->panel);
    if (err == ESP_OK && panel.bounce_buffer_size_px) {
        const esp_lcd_rgb_panel_event_callbacks_t callbacks = {.on_bounce_empty=fill_bounce};
        err = esp_lcd_rgb_panel_register_event_callbacks(d->panel, &callbacks, d);
    } else if (err == ESP_OK) {
        void *framebuffer = NULL;
        err = esp_lcd_rgb_panel_get_frame_buffer(d->panel, 1, &framebuffer);
        d->pixels = framebuffer;
    }
    if (err == ESP_OK) {
        for (size_t i = 0; i < count; i++) d->pixels[i] = d->background;
        err = esp_lcd_panel_reset(d->panel);
    }
    if (err == ESP_OK) err = esp_lcd_panel_init(d->panel);
    if (err == ESP_OK) err = flush(d);
    if (err == ESP_OK) {
        d->sleeping = false;
        err = backlight(d);
    }
    if (err != ESP_OK) {
        d->sleeping = true;
        (void)backlight(d);
        if (d->panel) (void)esp_lcd_panel_del(d->panel);
        if (panel.bounce_buffer_size_px) heap_caps_free(d->pixels);
        d->panel = NULL; d->pixels = NULL;
        if (d->config.have_power) (void)solar_os_gpio_line_write(&d->config.power, false);
    }
    return err;
}

static uint8_t display_cb(u8x8_t *u8x8, uint8_t message, uint8_t count, void *arg)
{
    solar_os_rgb_lcd_t *d = active;
    if (!d) return 0;
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &d->info);
        return 1;
    }
    if (message == U8X8_MSG_DISPLAY_INIT) return d->panel != NULL;
    if (message == U8X8_MSG_DISPLAY_SET_POWER_SAVE) {
        d->last_error = solar_os_rgb_lcd_power_save(d, count != 0);
        return d->last_error == ESP_OK;
    }
    if (message != U8X8_MSG_DISPLAY_DRAW_TILE || !d->panel || !arg) return 0;
    const u8x8_tile_t *tile = arg;
    const unsigned width = d->config.panel.timings.h_res;
    const unsigned height = d->config.panel.timings.v_res;
    for (unsigned n = 0; n < count; n++) {
        for (unsigned x = 0; x < (unsigned)tile->cnt * 8U; x++) {
            unsigned dx = ((unsigned)tile->x_pos + n * tile->cnt) * 8U + x;
            if (dx >= width) continue;
            const uint8_t bits = tile->tile_ptr[x];
            for (unsigned bit = 0; bit < 8; bit++) {
                unsigned dy = (unsigned)tile->y_pos * 8U + bit;
                if (dy < height) d->pixels[(size_t)dy * width + dx] =
                    bits & (1U << bit) ? d->foreground : d->background;
            }
        }
    }
    d->last_error = flush(d);
    return d->last_error == ESP_OK;
}

esp_err_t solar_os_rgb_lcd_init(solar_os_rgb_lcd_t *d, const solar_os_rgb_lcd_config_t *config)
{
    if (!d || !config) return ESP_ERR_INVALID_ARG;
    if (active) return ESP_ERR_INVALID_STATE;
    const esp_lcd_rgb_panel_config_t *p = &config->panel;
    if (!p->timings.h_res || !p->timings.v_res ||
        p->timings.h_res > 2040 || p->timings.v_res > 2040 ||
        p->timings.h_res % 8U || p->timings.v_res % 8U ||
        p->data_width != 16 || p->bits_per_pixel != 16 || p->num_fbs != 1 ||
        (p->bounce_buffer_size_px &&
         ((size_t)p->timings.h_res * p->timings.v_res) % p->bounce_buffer_size_px) ||
        !p->timings.pclk_hz || p->timings.pclk_hz > 40000000U)
        return ESP_ERR_INVALID_ARG;
    uint64_t pins = 0;
    for (unsigned i = 0; i < 20; i++) {
        const int pin = i < 16 ? p->data_gpio_nums[i] : i == 16 ? p->hsync_gpio_num :
            i == 17 ? p->vsync_gpio_num : i == 18 ? p->de_gpio_num : p->pclk_gpio_num;
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pin) || (pins & (1ULL << pin))) return ESP_ERR_INVALID_ARG;
        pins |= 1ULL << pin;
    }
    memset(d, 0, sizeof(*d));
    d->config = *config;
    d->foreground = 0; d->background = 0xffff; d->brightness = 100; d->sleeping = true;
    const size_t mono_size = (size_t)p->timings.h_res * p->timings.v_res / 8U;
    d->mono = heap_caps_calloc(1, mono_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d->mono) return ESP_ERR_NO_MEM;
    if (config->have_backlight) {
        const esp_err_t err = solar_os_gpio_line_write(&config->backlight, false);
        if (err != ESP_OK) { solar_os_rgb_lcd_deinit(d); return err; }
    }
    const esp_err_t err = start_panel(d);
    if (err != ESP_OK) { solar_os_rgb_lcd_deinit(d); return err; }
    d->info = (u8x8_display_info_t){
        .tile_width=p->timings.h_res/8U, .tile_height=p->timings.v_res/8U,
        .pixel_width=p->timings.h_res, .pixel_height=p->timings.v_res,
    };
    active = d;
    u8g2_SetupDisplay(&d->u8g2, display_cb, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
    u8g2_SetupBuffer(&d->u8g2, d->mono, d->info.tile_height,
        u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    u8g2_InitDisplay(&d->u8g2);
    return ESP_OK;
}

esp_err_t solar_os_rgb_lcd_shutdown(solar_os_rgb_lcd_t *d)
{
    if (!d) return ESP_ERR_INVALID_ARG;
    d->sleeping = true;
    esp_err_t err = backlight(d);
    if (err != ESP_OK) return err;
    if (d->panel) {
        err = esp_lcd_panel_del(d->panel);
        if (err != ESP_OK) return err;
        d->panel = NULL;
    }
    if (d->config.panel.bounce_buffer_size_px) heap_caps_free(d->pixels);
    d->pixels = NULL;
    return d->config.have_power ? solar_os_gpio_line_write(&d->config.power, false) : ESP_OK;
}

void solar_os_rgb_lcd_deinit(solar_os_rgb_lcd_t *d)
{
    if (!d) return;
    (void)solar_os_rgb_lcd_shutdown(d);
    heap_caps_free(d->mono); d->mono = NULL;
    if (active == d) active = NULL;
}

esp_err_t solar_os_rgb_lcd_resume(solar_os_rgb_lcd_t *d)
{
    if (!d || !d->mono) return ESP_ERR_INVALID_STATE;
    return d->panel ? solar_os_rgb_lcd_power_save(d, false) : start_panel(d);
}

esp_err_t solar_os_rgb_lcd_power_save(solar_os_rgb_lcd_t *d, bool enabled)
{
    if (!d) return ESP_ERR_INVALID_ARG;
    d->sleeping = enabled;
    return backlight(d);
}
esp_err_t solar_os_rgb_lcd_brightness(solar_os_rgb_lcd_t *d, uint8_t percent)
{
    if (!d || percent > 100) return ESP_ERR_INVALID_ARG;
    const uint8_t previous = d->brightness;
    d->brightness = percent;
    const esp_err_t err = backlight(d);
    if (err != ESP_OK) d->brightness = previous;
    return err;
}
esp_err_t solar_os_rgb_lcd_colors(solar_os_rgb_lcd_t *d, uint32_t fg, uint32_t bg)
{
    if (!d) return ESP_ERR_INVALID_ARG;
    d->foreground=((fg>>8)&0xf800U)|((fg>>5)&0x07e0U)|((fg>>3)&31U);
    d->background=((bg>>8)&0xf800U)|((bg>>5)&0x07e0U)|((bg>>3)&31U);
    return ESP_OK;
}
esp_err_t solar_os_rgb_lcd_frame(solar_os_rgb_lcd_t *d, const solar_os_display_raster_t *f)
{
    if (!d || !d->pixels) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = solar_os_rgb_lcd_render(d->pixels,
        d->info.pixel_width, d->info.pixel_height, f, d->foreground, d->background);
    return err == ESP_OK ? flush(d) : err;
}
esp_err_t solar_os_rgb_lcd_surface(solar_os_rgb_lcd_t *d, const solar_os_display_surface_t *s)
{
    if (!d || !d->pixels) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = solar_os_rgb_lcd_render_surface(d->pixels,
        d->info.pixel_width, d->info.pixel_height, s, d->foreground, d->background);
    return err == ESP_OK ? flush(d) : err;
}
