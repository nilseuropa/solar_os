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
#define ROW_ALIGNMENT 4U
#define TRANSFER_BYTES (WIDTH * ROW_ALIGNMENT * 2U)

struct solar_os_spd2010 {
    solar_os_spd2010_config_t config;
    solar_os_rgb_panel_buffer_t image;
    u8g2_t u8g2;
    u8x8_display_info_t info;
    uint8_t *mono;
    uint8_t dirty_rows[(HEIGHT + 7U) / 8U];
    uint32_t mono_hashes[(HEIGHT + 7U) / 8U];
    uint64_t mono_valid_rows;
    const uint8_t *surface_data;
    uint32_t surface_palette_hash;
    uint16_t surface_stride;
    solar_os_display_rotation_t surface_rotation;
    uint32_t frame_hashes[(HEIGHT + 7U) / 8U];
    uint64_t frame_valid_rows;
    uint32_t frame_palette_hash;
    uint16_t frame_width, frame_height;
    solar_os_display_rotation_t frame_rotation;
    bool frame_cached, frame_inverted;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    SemaphoreHandle_t done, mutex;
    StaticSemaphore_t done_storage, mutex_storage;
    uint16_t foreground, background;
    uint8_t brightness;
    bool spi_started, ready, sleeping, io_failed;
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

/* Keep the existing PSRAM image locked until the final queued DMA chunk ends.
 * The SDK pipelines bounded internal bounce buffers within one LCD window. */
static esp_err_t flush_rows(solar_os_spd2010_t *d, uint16_t first, uint16_t end)
{
    (void)xSemaphoreTake(d->done, 0);
    esp_err_t error = esp_lcd_panel_draw_bitmap(d->panel, 0, first, WIDTH, end,
        d->image.pixels + (size_t)first * WIDTH * 2U);
    if (error) {
        /* A later chunk can fail after earlier chunks were queued. Drain them
         * before the backing image can be modified or freed. No command is sent. */
        if (error != ESP_ERR_NO_MEM || esp_lcd_panel_io_tx_param(d->io, -1, NULL, 0) != ESP_OK) {
            /* Failed SPI completions can leave the SDK's pending count stale.
             * Do not wait for a completion already removed from its queue, or
             * reuse/free the retained image. A controller fault needs reboot. */
            d->io_failed = true;
            d->ready = false;
        }
        return error;
    }
    xSemaphoreTake(d->done, portMAX_DELAY);
    /* Reap completed descriptors now, releasing the SDK's temporary DMA
     * buffers instead of retaining them while the display is idle. */
    error = esp_lcd_panel_io_tx_param(d->io, -1, NULL, 0);
    if (error) { d->io_failed = true; d->ready = false; }
    return error;
}

static bool rows_dirty(const solar_os_spd2010_t *d, uint16_t y)
{
    return (d->dirty_rows[y / 8U] & (0x0fU << (y & 7U))) != 0;
}

static esp_err_t flush_changed_rows(solar_os_spd2010_t *d)
{
    for (uint16_t y = 0; y < HEIGHT;) {
        if (!rows_dirty(d, y)) { y += ROW_ALIGNMENT; continue; }
        const uint16_t first = y;
        do { y += ROW_ALIGNMENT; } while (y < HEIGHT && rows_dirty(d, y));
        const esp_err_t error = flush_rows(d, first, y);
        if (error) return error; /* Retain dirty rows so the next presentation retries. */
        for (uint16_t row = first; row < y; row += ROW_ALIGNMENT)
            d->dirty_rows[row / 8U] &= ~(0x0fU << (row & 7U));
    }
    return ESP_OK;
}

static solar_os_spd2010_t *from_u8x8(u8x8_t *u)
{
    return (solar_os_spd2010_t *)((uint8_t *)u - offsetof(solar_os_spd2010_t, u8g2) - offsetof(u8g2_t, u8x8));
}

static uint32_t hash_bytes(const uint8_t *data, size_t size)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < size; i++) hash = (hash ^ data[i]) * UINT32_C(16777619);
    return hash;
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
    if (message == U8X8_MSG_DISPLAY_REFRESH && d->ready) {
        xSemaphoreTake(d->mutex, portMAX_DELAY);
        d->last_error = flush_changed_rows(d);
        xSemaphoreGive(d->mutex);
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
    const uint64_t row_bit = UINT64_C(1) << tile->y_pos;
    const bool full_row = x0 == 0 && x1 == WIDTH;
    const uint32_t hash = full_row ? hash_bytes(tile->tile_ptr, WIDTH) : 0;
    if (full_row && (d->mono_valid_rows & row_bit) && d->mono_hashes[tile->y_pos] == hash) {
        xSemaphoreGive(d->mutex);
        return 1;
    }
    d->surface_data = NULL;
    d->frame_cached = false;
    for (uint16_t y = y0; y < y1; ++y) {
        for (uint16_t x = x0; x < x1; ++x) {
            uint16_t color = tile->tile_ptr[x - x0] & (1U << (y - y0)) ? d->background : d->foreground;
            solar_os_rgb_panel_put(&d->image, x, y, color);
        }
    }
    if (full_row) {
        d->mono_hashes[tile->y_pos] = hash;
        d->mono_valid_rows |= row_bit;
    } else d->mono_valid_rows &= ~row_bit;
    d->last_error = ESP_OK;
    xSemaphoreGive(d->mutex);
    return d->last_error == ESP_OK;
}

esp_err_t solar_os_spd2010_resume(solar_os_spd2010_t *d)
{
    if (!d || !d->panel || d->io_failed) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    d->ready = false;
    d->mono_valid_rows = 0;
    d->surface_data = NULL;
    d->frame_cached = false;
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
    if (!error) memset(d->dirty_rows, 0, sizeof(d->dirty_rows));
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
    if (!error && d->config.have_power && !d->config.keep_power_on_shutdown)
        error = control(&d->config.power, !d->config.power_active_high);
    if (!error) d->ready = false;
    return error;
}

esp_err_t solar_os_spd2010_destroy(solar_os_spd2010_t *d)
{
    if (!d) return ESP_OK;
    if (d->io_failed) return ESP_ERR_INVALID_STATE;
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
    d->image = (solar_os_rgb_panel_buffer_t){.width = WIDTH, .height = HEIGHT,
        .dirty_rows = d->dirty_rows};
    d->image.pixels = solar_os_memory_alloc(WIDTH * HEIGHT * 2U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "spd2010.frame");
    d->mono = solar_os_memory_calloc(((WIDTH + 7U) / 8U) * ((HEIGHT + 7U) / 8U), 8U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "spd2010.text");
    esp_err_t error = d->image.pixels && d->mono && d->done && d->mutex ? ESP_OK : ESP_ERR_NO_MEM;
    if (!error && c->backlight >= 0) {
        gpio_config_t gpio = {.pin_bit_mask = 1ULL << c->backlight, .mode = GPIO_MODE_OUTPUT};
        error = gpio_set_level(c->backlight, !c->backlight_active_high);
        if (!error) error = gpio_config(&gpio);
    }
    spi_bus_config_t bus = SPD2010_PANEL_BUS_QSPI_CONFIG(c->sclk,
        c->data[0], c->data[1], c->data[2], c->data[3], TRANSFER_BYTES);
    if (!error) error = spi_bus_initialize(c->host, &bus, SPI_DMA_CH_AUTO);
    if (!error) d->spi_started = true;
    esp_lcd_panel_io_spi_config_t io = SPD2010_PANEL_IO_QSPI_CONFIG(c->cs, color_done, d);
    io.pclk_hz = c->clock_hz; io.trans_queue_depth = 2;
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
    d->mono_valid_rows = 0;
    d->surface_data = NULL;
    const solar_os_display_rotation_t r = rotation(d);
    /* Scaled indexed canvases arrive as full-panel rasters without a change
     * map. Hash eight source rows at a time instead of reconverting the panel. */
    const bool indexed_canvas = f && f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 &&
        f->data && f->palette_rgb565 && f->palette_size >= 256U &&
        !f->x && !f->y && f->width == WIDTH && f->height == HEIGHT &&
        f->source_width && f->source_height && f->source_stride == f->source_width &&
        WIDTH % f->source_width == 0 && HEIGHT % f->source_height == 0 &&
        f->data_size >= (size_t)f->source_stride * f->source_height;
    esp_err_t error = ESP_OK;
    if (indexed_canvas) {
        const uint32_t palette_hash = hash_bytes((const uint8_t *)f->palette_rgb565,
            256U * sizeof(uint16_t));
        if (!d->frame_cached || d->frame_width != f->source_width ||
            d->frame_height != f->source_height || d->frame_rotation != r ||
            d->frame_inverted != f->palette_inverted || d->frame_palette_hash != palette_hash)
            d->frame_valid_rows = 0;
        d->frame_cached = true;
        d->frame_width = f->source_width; d->frame_height = f->source_height;
        d->frame_rotation = r; d->frame_inverted = f->palette_inverted;
        d->frame_palette_hash = palette_hash;
        const unsigned scale_y = HEIGHT / f->source_height;
        for (unsigned y = 0; y < f->source_height; y += 8U) {
            const unsigned rows = f->source_height - y < 8U ? f->source_height - y : 8U;
            const size_t offset = (size_t)y * f->source_stride;
            const uint32_t hash = hash_bytes(f->data + offset, rows * f->source_stride);
            const uint64_t bit = UINT64_C(1) << (y / 8U);
            if ((d->frame_valid_rows & bit) && d->frame_hashes[y / 8U] == hash) continue;
            solar_os_display_raster_t band = *f;
            band.data += offset; band.data_size -= offset;
            band.source_height = rows; band.y = y * scale_y; band.height = rows * scale_y;
            band.clear_background = false; /* These bands cover the entire panel. */
            error = solar_os_rgb_panel_frame(&d->image, &band, r);
            if (error) break;
            d->frame_hashes[y / 8U] = hash;
            d->frame_valid_rows |= bit;
        }
    } else {
        d->frame_cached = false;
        error = solar_os_rgb_panel_frame(&d->image, f, r);
    }
    if (!error) error = flush_changed_rows(d);
    xSemaphoreGive(d->mutex);
    return error;
}

esp_err_t solar_os_spd2010_surface(void *ctx, const solar_os_display_surface_t *s)
{
    solar_os_spd2010_t *d = ctx;
    if (!d || !d->ready) return ESP_ERR_INVALID_STATE;
    if (!s) return ESP_ERR_INVALID_ARG;
    /* Full-redraw apps mark every tile dirty. Reuse the raster row cache so
     * unchanged areas need only a linear source-byte hash, not tile gathering. */
    if (s->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 && s->width == WIDTH && s->height == HEIGHT &&
        s->native_width == WIDTH && s->native_height == HEIGHT && s->stride == WIDTH &&
        s->rotation == rotation(d) && s->dirty_tiles && s->tile_size == 8U &&
        s->dirty_stride >= 7U && s->dirty_size >= (size_t)s->dirty_stride * 52U) {
        bool full = true;
        for (unsigned y = 0; y < 52U && full; y++) for (unsigned x = 0; x < 7U; x++) {
            const uint8_t mask = x == 6U ? 0x0fU : 0xffU;
            if ((s->dirty_tiles[(size_t)y * s->dirty_stride + x] & mask) != mask) { full = false; break; }
        }
        if (full) {
            const solar_os_display_raster_t f = {.data=s->data,.data_size=s->data_size,
                .palette_rgb565=s->palette_rgb565,.palette_size=s->palette_size,
                .source_width=WIDTH,.source_height=HEIGHT,.source_stride=WIDTH,
                .width=WIDTH,.height=HEIGHT,.format=SOLAR_OS_DISPLAY_FORMAT_INDEX8};
            return solar_os_spd2010_frame(d, &f);
        }
    }
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    d->mono_valid_rows = 0;
    d->frame_cached = false;
    const uint32_t palette_hash = s->palette_rgb565 && s->palette_size >= 256U ?
        hash_bytes((const uint8_t *)s->palette_rgb565, 256U * sizeof(uint16_t)) : 0;
    const bool changed_only = d->surface_data == s->data && d->surface_stride == s->stride &&
        d->surface_rotation == s->rotation && d->surface_palette_hash == palette_hash;
    if (!changed_only && s->presented_hashes && s->hash_stride >= 52U &&
        s->presented_hash_count >= (size_t)s->hash_stride * 52U)
        for (unsigned y = 0; y < 52U; y++)
            memset(s->presented_hashes + (size_t)y * s->hash_stride, 0, 52U * sizeof(uint32_t));
    /* Dirty tiles are patches, including UI over a directly presented video.
     * Invalidating hashes must not repaint unmarked video pixels. */
    esp_err_t error = solar_os_rgb_panel_surface_update(&d->image, s, true);
    if (!error) {
        d->surface_data = s->data;
        d->surface_stride = s->stride;
        d->surface_rotation = s->rotation;
        d->surface_palette_hash = palette_hash;
    }
    if (!error) error = flush_changed_rows(d);
    xSemaphoreGive(d->mutex);
    return error;
}

esp_err_t solar_os_spd2010_colors(solar_os_spd2010_t *d, uint32_t foreground, uint32_t background)
{
    if (!d || foreground > 0xffffff || background > 0xffffff) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    d->mono_valid_rows = 0;
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
