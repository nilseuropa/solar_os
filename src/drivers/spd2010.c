#include "spd2010.h"

#include <stddef.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_spd2010.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pwm_port.h"
#include "solar_os_memory.h"
#include "solar_os_rgb_panel_buffer.h"

#define WIDTH 412U
#define HEIGHT 412U
#define BAND_ROWS 4U

struct solar_os_spd2010 {
    solar_os_spd2010_config_t config;
    solar_os_rgb_panel_buffer_t image;
    u8g2_t u8g2;
    u8x8_display_info_t info;
    uint8_t *mono, *dma;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    SemaphoreHandle_t done, mutex;
    StaticSemaphore_t done_storage, mutex_storage;
    uint16_t foreground, background;
    uint8_t brightness;
    bool spi_started, ready, sleeping;
    esp_err_t last_error;
};

static bool IRAM_ATTR color_done(esp_lcd_panel_io_handle_t io,
    esp_lcd_panel_io_event_data_t *event, void *user)
{
    (void)io; (void)event;
    solar_os_spd2010_t *d = user;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(d->done, &wake);
    return wake == pdTRUE;
}

static esp_err_t control(const solar_os_gpio_line_ref_t *line, bool level)
{
    /* Preload the latch before enabling the output, including native pins. */
    esp_err_t error = solar_os_gpio_line_write(line, level);
    if (!error) error = solar_os_gpio_line_configure(line,
        SOLAR_OS_GPIO_LINE_MODE_OUTPUT, SOLAR_OS_GPIO_LINE_PULL_NONE);
    return error;
}

static esp_err_t backlight(solar_os_spd2010_t *d, bool enabled)
{
    if (d->config.backlight < 0) return ESP_OK;
    uint8_t percent = enabled ? d->brightness : 0;
    if (d->config.backlight_pwm)
        return pwm_port_set(d->config.backlight, 20000,
            d->config.backlight_active_high ? percent : 100U - percent);
    return gpio_set_level(d->config.backlight,
        percent ? d->config.backlight_active_high : !d->config.backlight_active_high);
}

/* The staging allocation stays pinned until the SPI completion callback. Every
 * rectangle spans full rows, satisfying the controller's four-pixel alignment. */
static esp_err_t flush_rows(solar_os_spd2010_t *d, uint16_t first, uint16_t end)
{
    for (uint16_t y = first; y < end; y += BAND_ROWS) {
        uint16_t rows = end - y < BAND_ROWS ? end - y : BAND_ROWS;
        memcpy(d->dma, d->image.pixels + (size_t)y * WIDTH * 2U, (size_t)rows * WIDTH * 2U);
        (void)xSemaphoreTake(d->done, 0);
        esp_err_t error = esp_lcd_panel_draw_bitmap(d->panel, 0, y, WIDTH, y + rows, d->dma);
        if (error) return error;
        /* SPI has no peripheral acknowledgement; completion depends on the
         * bounded DMA transfer, even when the display is disconnected. */
        xSemaphoreTake(d->done, portMAX_DELAY);
    }
    return ESP_OK;
}

static solar_os_spd2010_t *from_u8x8(u8x8_t *u)
{
    return (solar_os_spd2010_t *)((uint8_t *)u - offsetof(solar_os_spd2010_t, u8g2) - offsetof(u8g2_t, u8x8));
}

static uint8_t display_callback(u8x8_t *u, uint8_t message, uint8_t count, void *arg)
{
    solar_os_spd2010_t *d = from_u8x8(u);
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u, &d->info); return 1;
    }
    if (message == U8X8_MSG_DISPLAY_INIT) return d->ready;
    if (message == U8X8_MSG_DISPLAY_SET_POWER_SAVE) {
        d->last_error = solar_os_spd2010_power_save(d, count != 0);
        return d->last_error == ESP_OK;
    }
    if (message != U8X8_MSG_DISPLAY_DRAW_TILE || !arg || !d->ready) return 0;
    const u8x8_tile_t *tile = arg;
    if (!tile->tile_ptr || !tile->cnt || tile->x_pos * 8U >= WIDTH || tile->y_pos * 8U >= HEIGHT)
        return 0;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    uint16_t y0 = (uint16_t)tile->y_pos * 8U;
    uint16_t x0 = (uint16_t)tile->x_pos * 8U;
    uint16_t y1 = y0 + 8U < HEIGHT ? y0 + 8U : HEIGHT;
    uint16_t x1 = x0 + (uint16_t)tile->cnt * 8U;
    if (x1 > WIDTH) x1 = WIDTH;
    for (uint16_t y = y0; y < y1; ++y) {
        for (uint16_t x = x0; x < x1; ++x) {
            uint16_t color = tile->tile_ptr[x - x0] & (1U << (y - y0)) ? d->background : d->foreground;
            size_t offset = ((size_t)y * WIDTH + x) * 2U;
            d->image.pixels[offset] = (uint8_t)(color >> 8);
            d->image.pixels[offset + 1U] = (uint8_t)color;
        }
    }
    d->last_error = y0 < HEIGHT && x0 < WIDTH && tile->cnt && tile->tile_ptr ?
        flush_rows(d, y0, y1) : ESP_ERR_INVALID_ARG;
    xSemaphoreGive(d->mutex);
    return d->last_error == ESP_OK;
}

esp_err_t solar_os_spd2010_resume(solar_os_spd2010_t *d)
{
    if (!d || !d->panel) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    d->ready = false;
    esp_err_t error = backlight(d, false);
    if (!error && d->config.have_power) error = control(&d->config.power, d->config.power_active_high);
    if (!error) vTaskDelay(pdMS_TO_TICKS(20) + 1);
    if (!error && d->config.have_reset) {
        error = control(&d->config.reset, false);
        vTaskDelay(pdMS_TO_TICKS(10) + 1);
        if (!error) error = control(&d->config.reset, true);
        vTaskDelay(pdMS_TO_TICKS(120) + 1);
    }
    if (!error) error = esp_lcd_panel_reset(d->panel);
    if (!error) error = esp_lcd_panel_init(d->panel);
    /* Reapply address direction after the vendor initialization sequence,
     * matching the panel supplier's startup procedure. */
    if (!error) error = esp_lcd_panel_mirror(d->panel, false, false);
    if (!error && d->config.invert) error = esp_lcd_panel_invert_color(d->panel, true);
    if (!error) error = flush_rows(d, 0, HEIGHT);
    if (!error) error = esp_lcd_panel_disp_on_off(d->panel, true);
    if (!error) error = backlight(d, true);
    d->ready = error == ESP_OK;
    d->sleeping = false;
    d->last_error = error;
    xSemaphoreGive(d->mutex);
    return error;
}

esp_err_t solar_os_spd2010_power_save(solar_os_spd2010_t *d, bool enabled)
{
    if (!d || !d->ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    esp_err_t error = enabled ? backlight(d, false) : ESP_OK;
    if (!error) error = esp_lcd_panel_disp_on_off(d->panel, !enabled);
    if (!error && !enabled) error = backlight(d, true);
    if (!error) d->sleeping = enabled;
    xSemaphoreGive(d->mutex);
    return error;
}

esp_err_t solar_os_spd2010_shutdown(solar_os_spd2010_t *d)
{
    esp_err_t error = solar_os_spd2010_power_save(d, true);
    if (!error && d->config.have_power) error = control(&d->config.power, !d->config.power_active_high);
    if (!error) d->ready = false;
    return error;
}

esp_err_t solar_os_spd2010_destroy(solar_os_spd2010_t *d)
{
    if (!d) return ESP_OK;
    (void)backlight(d, false);
    if (d->panel) {
        esp_err_t error = esp_lcd_panel_del(d->panel);
        if (error) return error;
        d->panel = NULL;
    }
    if (d->io) {
        esp_err_t error = esp_lcd_panel_io_del(d->io);
        if (error) return error;
        d->io = NULL;
    }
    if (d->spi_started) {
        esp_err_t error = spi_bus_free(d->config.host);
        if (error) return error;
        d->spi_started = false;
    }
    if (d->config.backlight >= 0) {
        if (d->config.backlight_pwm) (void)pwm_port_stop(d->config.backlight);
        (void)gpio_reset_pin(d->config.backlight);
    }
    if (d->config.have_power) (void)control(&d->config.power, !d->config.power_active_high);
    solar_os_memory_free(d->dma);
    solar_os_memory_free(d->mono);
    solar_os_memory_free(d->image.pixels);
    if (d->done) vSemaphoreDelete(d->done);
    if (d->mutex) vSemaphoreDelete(d->mutex);
    solar_os_memory_free(d);
    return ESP_OK;
}

esp_err_t solar_os_spd2010_create(const solar_os_spd2010_config_t *c, solar_os_spd2010_t **out)
{
    if (!c || !out || c->rotation > 3 || c->clock_hz < 100000 || c->clock_hz > 40000000 ||
        (c->host != SPI2_HOST && c->host != SPI3_HOST)) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    int pins[] = {c->sclk, c->data[0], c->data[1], c->data[2], c->data[3], c->cs, c->backlight};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if ((i < 6 || pins[i] >= 0) && !GPIO_IS_VALID_OUTPUT_GPIO(pins[i])) return ESP_ERR_INVALID_ARG;
        for (size_t j = i + 1; j < sizeof(pins) / sizeof(pins[0]); ++j)
            if (pins[i] >= 0 && pins[i] == pins[j]) return ESP_ERR_INVALID_ARG;
    }
    solar_os_spd2010_t *d = solar_os_memory_calloc(1, sizeof(*d), SOLAR_OS_MEMORY_INTERNAL_CRITICAL, "spd2010.state");
    if (!d) return ESP_ERR_NO_MEM;
    d->config = *c;
    d->background = 0xffff; d->brightness = 100;
    d->done = xSemaphoreCreateBinaryStatic(&d->done_storage);
    d->mutex = xSemaphoreCreateMutexStatic(&d->mutex_storage);
    d->image = (solar_os_rgb_panel_buffer_t){.width = WIDTH, .height = HEIGHT};
    d->image.pixels = solar_os_memory_alloc(WIDTH * HEIGHT * 2U, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "spd2010.frame");
    d->mono = solar_os_memory_calloc(((WIDTH + 7U) / 8U) * ((HEIGHT + 7U) / 8U), 8U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "spd2010.text");
    d->dma = solar_os_memory_alloc(WIDTH * BAND_ROWS * 2U, SOLAR_OS_MEMORY_DMA, "spd2010.dma");
    esp_err_t error = d->image.pixels && d->mono && d->dma && d->done && d->mutex ? ESP_OK : ESP_ERR_NO_MEM;
    if (!error && c->backlight >= 0) {
        gpio_config_t gpio = {.pin_bit_mask = 1ULL << c->backlight, .mode = GPIO_MODE_OUTPUT};
        error = gpio_set_level(c->backlight, !c->backlight_active_high);
        if (!error) error = gpio_config(&gpio);
    }
    spi_bus_config_t bus = SPD2010_PANEL_BUS_QSPI_CONFIG(c->sclk,
        c->data[0], c->data[1], c->data[2], c->data[3], WIDTH * BAND_ROWS * 2U);
    if (!error) error = spi_bus_initialize(c->host, &bus, SPI_DMA_CH_AUTO);
    if (!error) d->spi_started = true;
    esp_lcd_panel_io_spi_config_t io = SPD2010_PANEL_IO_QSPI_CONFIG(c->cs, color_done, d);
    io.pclk_hz = c->clock_hz; io.trans_queue_depth = 1;
    if (!error) error = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)c->host, &io, &d->io);
    spd2010_vendor_config_t vendor = {.flags.use_qspi_interface = true};
    esp_lcd_panel_dev_config_t panel = {.reset_gpio_num = -1,
        .rgb_ele_order = c->bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16, .vendor_config = &vendor};
    if (!error) error = esp_lcd_new_panel_spd2010(d->io, &panel, &d->panel);
    if (!error) {
        solar_os_rgb_panel_clear(&d->image, d->background);
        error = solar_os_spd2010_resume(d);
    }
    if (error) { (void)solar_os_spd2010_destroy(d); return error; }
    d->info = (u8x8_display_info_t){.chip_enable_level = 0, .chip_disable_level = 1,
        .sck_clock_hz = c->clock_hz, .tile_width = (WIDTH + 7U) / 8U,
        .tile_height = (HEIGHT + 7U) / 8U, .pixel_width = WIDTH, .pixel_height = HEIGHT};
    const u8g2_cb_t *rotations[] = {U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3};
    u8g2_SetupDisplay(&d->u8g2, display_callback, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
    u8g2_SetupBuffer(&d->u8g2, d->mono, d->info.tile_height,
        u8g2_ll_hvline_vertical_top_lsb, rotations[c->rotation]);
    u8g2_InitDisplay(&d->u8g2);
    *out = d;
    return ESP_OK;
}

u8g2_t *solar_os_spd2010_u8g2(solar_os_spd2010_t *d) { return d ? &d->u8g2 : NULL; }

static solar_os_display_rotation_t rotation(const solar_os_spd2010_t *d)
{
    const u8g2_cb_t *r = d->u8g2.cb;
    return r == U8G2_R1 ? SOLAR_OS_DISPLAY_ROTATION_90 : r == U8G2_R2 ?
        SOLAR_OS_DISPLAY_ROTATION_180 : r == U8G2_R3 ? SOLAR_OS_DISPLAY_ROTATION_270 : SOLAR_OS_DISPLAY_ROTATION_0;
}

esp_err_t solar_os_spd2010_frame(void *ctx, const solar_os_display_raster_t *f)
{
    solar_os_spd2010_t *d = ctx;
    if (!d || !d->ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    esp_err_t error = solar_os_rgb_panel_frame(&d->image, f, rotation(d));
    if (!error) error = flush_rows(d, 0, HEIGHT);
    xSemaphoreGive(d->mutex);
    return error;
}

esp_err_t solar_os_spd2010_surface(void *ctx, const solar_os_display_surface_t *s)
{
    solar_os_spd2010_t *d = ctx;
    if (!d || !d->ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    esp_err_t error = solar_os_rgb_panel_surface(&d->image, s);
    if (!error) error = flush_rows(d, 0, HEIGHT);
    xSemaphoreGive(d->mutex);
    return error;
}

esp_err_t solar_os_spd2010_colors(solar_os_spd2010_t *d, uint32_t foreground, uint32_t background)
{
    if (!d || foreground > 0xffffff || background > 0xffffff) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    d->foreground = ((foreground >> 8) & 0xf800) | ((foreground >> 5) & 0x7e0) | ((foreground >> 3) & 0x1f);
    d->background = ((background >> 8) & 0xf800) | ((background >> 5) & 0x7e0) | ((background >> 3) & 0x1f);
    xSemaphoreGive(d->mutex);
    return ESP_OK;
}

bool solar_os_spd2010_has_backlight(const solar_os_spd2010_t *d) { return d && d->config.backlight >= 0; }
esp_err_t solar_os_spd2010_get_brightness(const solar_os_spd2010_t *d, uint8_t *percent)
{
    if (!d || !percent) return ESP_ERR_INVALID_ARG;
    if (!solar_os_spd2010_has_backlight(d)) return ESP_ERR_NOT_SUPPORTED;
    *percent = d->brightness; return ESP_OK;
}
esp_err_t solar_os_spd2010_set_brightness(solar_os_spd2010_t *d, uint8_t percent)
{
    if (!d || percent > 100) return ESP_ERR_INVALID_ARG;
    if (!solar_os_spd2010_has_backlight(d)) return ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    uint8_t old = d->brightness; d->brightness = percent;
    esp_err_t error = backlight(d, d->ready && !d->sleeping);
    if (error) d->brightness = old;
    xSemaphoreGive(d->mutex);
    return error;
}
