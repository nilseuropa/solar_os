#include "epd_it8951.h"

#include <stddef.h>
#include <string.h>
#include "driver/gpio.h"
#include "solar_os_memory.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

#define WIDTH SOLAR_OS_IT8951_WIDTH
#define HEIGHT SOLAR_OS_IT8951_HEIGHT
#define MONO_BYTES ((size_t)WIDTH * ((HEIGHT + 7U) / 8U))
#define GRAY_BYTES ((size_t)WIDTH * HEIGHT / 2U)
#define TRY(call) do { esp_err_t err_ = (call); if (err_ != ESP_OK) return err_; } while (0)

static const char *TAG = "it8951";
static const u8x8_display_info_t display_info = {
    .chip_enable_level = 0, .chip_disable_level = 1,
    .sck_clock_hz = 10000000, .spi_mode = 0,
    .tile_width = (WIDTH + 7U) / 8U, .tile_height = (HEIGHT + 7U) / 8U,
    .pixel_width = WIDTH, .pixel_height = HEIGHT,
};

static void delay_ms(unsigned ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t wait_ready(solar_os_it8951_t *d)
{
    const int64_t deadline = esp_timer_get_time() + 2000000;
    while (!gpio_get_level(d->config.busy)) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        delay_ms(1);
    }
    return ESP_OK;
}

/* CS is managed explicitly across preamble, ready waits, dummy and payload.
 * Acquire the shared SPI bus only for this transaction, never while refreshing. */
static esp_err_t transfer(solar_os_it8951_t *d, uint16_t preamble,
                           const uint8_t *tx, uint8_t *rx, size_t size)
{
    TRY(wait_ready(d));
    TRY(spi_device_acquire_bus(d->spi, portMAX_DELAY));
    esp_err_t err = gpio_set_level(d->config.cs, 0);
    const uint8_t header[] = {(uint8_t)(preamble >> 8), (uint8_t)preamble};
    spi_transaction_t t = {.length = 16, .tx_buffer = header};
    if (err == ESP_OK) err = spi_device_polling_transmit(d->spi, &t);
    if (err == ESP_OK) err = wait_ready(d);
    if (err == ESP_OK && rx) {
        const uint8_t dummy[2] = {0};
        t = (spi_transaction_t){.length = 16, .tx_buffer = dummy};
        err = spi_device_polling_transmit(d->spi, &t);
        if (err == ESP_OK) err = wait_ready(d);
    }
    if (err == ESP_OK) {
        t = (spi_transaction_t){.length = size * 8U,
            .tx_buffer = tx, .rx_buffer = rx};
        err = spi_device_polling_transmit(d->spi, &t);
    }
    const esp_err_t cs_err = gpio_set_level(d->config.cs, 1);
    spi_device_release_bus(d->spi);
    return err == ESP_OK ? cs_err : err;
}

static esp_err_t word(solar_os_it8951_t *d, uint16_t preamble, uint16_t value)
{
    const uint8_t bytes[] = {(uint8_t)(value >> 8), (uint8_t)value};
    return transfer(d, preamble, bytes, NULL, sizeof(bytes));
}

static esp_err_t command(solar_os_it8951_t *d, uint16_t cmd,
                          const uint16_t *args, size_t count)
{
    TRY(word(d, 0x6000, cmd));
    for (size_t i = 0; i < count; i++) TRY(word(d, 0, args[i]));
    return ESP_OK;
}

static esp_err_t write_reg(solar_os_it8951_t *d, uint16_t reg, uint16_t value)
{
    const uint16_t args[] = {reg, value};
    return command(d, 0x0011, args, 2);
}

static esp_err_t read_reg(solar_os_it8951_t *d, uint16_t reg, uint16_t *value)
{
    TRY(command(d, 0x0010, &reg, 1));
    uint8_t data[2];
    TRY(transfer(d, 0x1000, NULL, data, sizeof(data)));
    *value = (uint16_t)((uint16_t)data[0] << 8) | data[1];
    return ESP_OK;
}

static esp_err_t wait_lut(solar_os_it8951_t *d)
{
    const int64_t deadline = esp_timer_get_time() + 30000000;
    for (;;) {
        uint16_t busy;
        TRY(read_reg(d, 0x1224, &busy));
        if (!busy) return ESP_OK;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        delay_ms(10);
    }
}

static esp_err_t rails(solar_os_it8951_t *d, bool on)
{
    const esp_err_t panel_err = gpio_set_level(d->config.power, on);
    const esp_err_t controller_err = gpio_set_level(d->config.ite_power, on);
    if (panel_err != ESP_OK || controller_err != ESP_OK) {
        d->ready = d->shadow_valid = false;
        /* Attempt both disables even if one GPIO operation failed. */
        const esp_err_t panel_off = gpio_set_level(d->config.power, 0);
        const esp_err_t controller_off = gpio_set_level(d->config.ite_power, 0);
        d->powered = panel_off != ESP_OK || controller_off != ESP_OK;
        return panel_err != ESP_OK ? panel_err : controller_err;
    }
    d->powered = on;
    if (!on) d->ready = d->shadow_valid = false;
    return ESP_OK;
}

static esp_err_t controller_init(solar_os_it8951_t *d)
{
    d->ready = d->shadow_valid = false;
    TRY(rails(d, true));
    delay_ms(50);
    TRY(gpio_set_level(d->config.reset, 0));
    delay_ms(10);
    TRY(gpio_set_level(d->config.reset, 1));
    delay_ms(50);
    TRY(command(d, 0x0001, NULL, 0));
    TRY(command(d, 0x0302, NULL, 0));
    uint8_t info[40];
    TRY(transfer(d, 0x1000, NULL, info, sizeof(info)));
    const unsigned width = ((unsigned)info[0] << 8) | info[1];
    const unsigned height = ((unsigned)info[2] << 8) | info[3];
    if (width != WIDTH || height != HEIGHT) {
        ESP_LOGE(TAG, "unexpected panel %ux%u (expected %ux%u)", width, height, WIDTH, HEIGHT);
        return ESP_ERR_INVALID_RESPONSE;
    }
    d->image_address = ((uint32_t)info[6] << 24) | ((uint32_t)info[7] << 16) |
                       ((uint32_t)info[4] << 8) | info[5];
    if (!d->image_address || d->image_address == UINT32_MAX) return ESP_ERR_INVALID_RESPONSE;
    const uint16_t vcom[] = {2, d->config.vcom_mv};
    TRY(command(d, 0x0039, vcom, 2));
    TRY(write_reg(d, 0x0004, 1)); /* packed SPI host interface */
    /* ED103TC2 requires a waveform temperature. Match Seeed's panel profile;
     * do not write a Waveshare panel's VCOM or LUT to this glass. */
    const uint16_t temperature[] = {1, 16};
    TRY(command(d, 0x0040, temperature, 2));
    d->ready = true;
    d->updates = 0;
    ESP_LOGI(TAG, "ED103TC2 %ux%u image=0x%08lx VCOM=-%umV",
             WIDTH, HEIGHT, (unsigned long)d->image_address, d->config.vcom_mv);
    return ESP_OK;
}

static uint8_t gray_at(const solar_os_it8951_t *d, unsigned x, unsigned y)
{
    const uint8_t b = d->gray[(size_t)y * (WIDTH / 2U) + x / 2U];
    return (x & 1U) ? b & 15U : b >> 4;
}

static void set_pixel(solar_os_it8951_t *d, unsigned x, unsigned y, uint8_t gray)
{
    uint8_t *b = &d->gray[(size_t)y * (WIDTH / 2U) + x / 2U];
    *b = (x & 1U) ? (uint8_t)((*b & 0xf0U) | gray) :
                    (uint8_t)((*b & 0x0fU) | (gray << 4));
    b = &d->mono[(size_t)(y / 8U) * WIDTH + x];
    const uint8_t mask = (uint8_t)(1U << (y & 7U));
    if (gray >= 8U) *b |= mask;
    else *b &= (uint8_t)~mask;
}

static void mono_to_gray(solar_os_it8951_t *d)
{
    for (unsigned y = 0; y < HEIGHT; y++) {
        const uint8_t *row = d->mono + (size_t)(y / 8U) * WIDTH;
        const uint8_t mask = (uint8_t)(1U << (y & 7U));
        for (unsigned x = 0; x < WIDTH; x += 2U)
            d->gray[(size_t)y * (WIDTH / 2U) + x / 2U] =
                (uint8_t)((row[x] & mask ? 0xf0 : 0) | (row[x + 1U] & mask ? 0x0f : 0));
    }
}

static esp_err_t refresh(solar_os_it8951_t *d)
{
    if (!d->ready) TRY(controller_init(d));
    if (d->shadow_valid && !d->full_refresh && !memcmp(d->gray, d->shadow, GRAY_BYTES))
        return ESP_OK;
    TRY(wait_lut(d));
    TRY(write_reg(d, 0x020a, (uint16_t)(d->image_address >> 16)));
    TRY(write_reg(d, 0x0208, (uint16_t)d->image_address));
    /* Use the vendor-proven 8bpp transfer mode for both mono and 16 gray
     * levels. Internal storage remains packed 4bpp. ED103TC2 mirrors X. */
    const uint16_t load[] = {0x0130, 0, 0, WIDTH, HEIGHT};
    TRY(command(d, 0x0021, load, 5));
    bool binary = true;
    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < WIDTH; x++) {
            const uint8_t g = gray_at(d, WIDTH - 1U - x, y);
            binary = binary && (g == 0 || g == 15);
            d->transfer[x] = (uint8_t)(g * 17U);
        }
        TRY(transfer(d, 0, d->transfer, NULL, WIDTH));
        if ((y & 31U) == 0) delay_ms(1);
    }
    TRY(command(d, 0x0022, NULL, 0));
    const bool full = d->full_refresh || !d->shadow_valid || !binary ||
        !d->shadow_binary || d->updates >= 19U;
    const uint16_t area[] = {0, 0, WIDTH, HEIGHT, full ? 2 : 1};
    TRY(command(d, 0x0034, area, 5));
    TRY(wait_lut(d));
    memcpy(d->shadow, d->gray, GRAY_BYTES);
    d->shadow_valid = true;
    d->shadow_binary = binary;
    d->updates = full ? 0 : d->updates + 1U;
    return ESP_OK;
}

static esp_err_t finish_refresh(solar_os_it8951_t *d)
{
    const esp_err_t err = refresh(d);
    if (err != ESP_OK) {
        /* A partially transferred frame must never become the comparison baseline. */
        d->ready = d->shadow_valid = false;
        ESP_LOGE(TAG, "refresh failed: %s", esp_err_to_name(err));
    }
    d->last_error = err;
    return err;
}

esp_err_t solar_os_it8951_present(solar_os_it8951_t *d)
{
    if (!d || !d->spi || !d->mono) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    mono_to_gray(d);
    const esp_err_t err = finish_refresh(d);
    xSemaphoreGive(d->mutex);
    return err;
}

esp_err_t solar_os_it8951_set_power_save(solar_os_it8951_t *d, bool sleep)
{
    if (!d || !d->spi) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (sleep) {
        if (d->ready) {
            err = wait_lut(d);
            if (err == ESP_OK) err = command(d, 0x0003, NULL, 0);
        }
        if (err == ESP_OK) err = rails(d, false);
    } else if (!d->ready) err = controller_init(d);
    d->last_error = err;
    xSemaphoreGive(d->mutex);
    return err;
}

static uint8_t display_cb(u8x8_t *u8x8, uint8_t message, uint8_t arg, void *ptr)
{
    (void)ptr;
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &display_info);
        return 1;
    }
    solar_os_it8951_t *d = (void *)((uint8_t *)u8x8 -
        offsetof(solar_os_it8951_t, u8g2) - offsetof(u8g2_t, u8x8));
    if (message == U8X8_MSG_DISPLAY_DRAW_TILE) return 1;
    if (message == U8X8_MSG_DISPLAY_REFRESH) return solar_os_it8951_present(d) == ESP_OK;
    if (message == U8X8_MSG_DISPLAY_SET_POWER_SAVE)
        return solar_os_it8951_set_power_save(d, arg != 0) == ESP_OK;
    if (message == U8X8_MSG_DISPLAY_INIT)
        return solar_os_it8951_set_power_save(d, false) == ESP_OK;
    return 0;
}

esp_err_t solar_os_it8951_init(solar_os_it8951_t *d, const solar_os_it8951_config_t *cfg)
{
    if (!d || !cfg || !cfg->spi_bus || !cfg->spi_bus[0] || cfg->rotation > 3 ||
        cfg->clock_hz < 100000U || cfg->clock_hz > 10000000U || cfg->vcom_mv != 1400)
        return ESP_ERR_INVALID_ARG;
    const int pins[] = {cfg->cs, cfg->reset, cfg->busy, cfg->power, cfg->ite_power};
    for (unsigned i = 0; i < 5; i++) {
        if (!GPIO_IS_VALID_GPIO(pins[i]) || (i != 2 && !GPIO_IS_VALID_OUTPUT_GPIO(pins[i])))
            return ESP_ERR_INVALID_ARG;
        for (unsigned j = i + 1; j < 5; j++) if (pins[i] == pins[j]) return ESP_ERR_INVALID_ARG;
    }
    memset(d, 0, sizeof(*d));
    d->config = *cfg;
    /* No caller-owned strings are retained after initialization. */
    d->config.spi_bus = NULL;
    d->mutex = xSemaphoreCreateMutexStatic(&d->mutex_storage);
    if (!d->mutex) return ESP_ERR_NO_MEM;
    const gpio_config_t outputs = {.pin_bit_mask = (1ULL << cfg->cs) | (1ULL << cfg->reset) |
        (1ULL << cfg->power) | (1ULL << cfg->ite_power), .mode = GPIO_MODE_OUTPUT};
    TRY(gpio_config(&outputs));
    TRY(gpio_set_level(cfg->cs, 1));
    const gpio_config_t busy = {.pin_bit_mask = 1ULL << cfg->busy,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    TRY(gpio_config(&busy));
    const spi_device_interface_config_t spi_cfg = {.clock_speed_hz = (int)cfg->clock_hz,
        .mode = 0, .spics_io_num = -1, .queue_size = 1};
    esp_err_t err = solar_os_bus_spi_add_device(cfg->spi_bus, &spi_cfg, &d->spi);
    if (err != ESP_OK) return err;
    d->mono = solar_os_memory_calloc(1, MONO_BYTES, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "it8951.mono");
    d->gray = solar_os_memory_alloc(GRAY_BYTES, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "it8951.gray");
    d->shadow = solar_os_memory_alloc(GRAY_BYTES, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "it8951.shadow");
    d->transfer = solar_os_memory_alloc(WIDTH, SOLAR_OS_MEMORY_DMA, "it8951.spi");
    if (!d->mono || !d->gray || !d->shadow || !d->transfer) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        /* SolarOS's native monochrome convention is one=white, zero=black.
         * Raster MONO1 input uses its separate, explicit palette convention. */
        memset(d->mono, 0xff, MONO_BYTES);
        memset(d->gray, 0xff, GRAY_BYTES);
        static const u8g2_cb_t *const rotations[] = {U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3};
        u8g2_SetupDisplay(&d->u8g2, display_cb, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
        u8g2_SetupBuffer(&d->u8g2, d->mono, (HEIGHT + 7U) / 8U,
            u8g2_ll_hvline_vertical_top_lsb, rotations[cfg->rotation]);
        u8g2_InitDisplay(&d->u8g2);
        err = d->last_error;
    }
    if (err != ESP_OK) solar_os_it8951_deinit(d);
    return err;
}

void solar_os_it8951_deinit(solar_os_it8951_t *d)
{
    if (!d) return;
    if (d->powered && d->spi) (void)solar_os_it8951_set_power_save(d, true);
    if (d->powered) (void)rails(d, false);
    if (d->spi) spi_bus_remove_device(d->spi);
    solar_os_memory_free(d->mono); solar_os_memory_free(d->gray);
    solar_os_memory_free(d->shadow); solar_os_memory_free(d->transfer);
    memset(d, 0, sizeof(*d));
}

const char *solar_os_it8951_mode(const solar_os_it8951_t *d)
{
    return d && d->full_refresh ? "refresh=full grayscale=16" : "refresh=auto grayscale=16";
}

esp_err_t solar_os_it8951_set_mode(solar_os_it8951_t *d, const char *mode)
{
    if (!d || !d->spi || !mode) return ESP_ERR_INVALID_ARG;
    const bool full = !strcmp(mode, "refresh=full");
    if (!full && strcmp(mode, "refresh=auto")) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    d->full_refresh = full;
    xSemaphoreGive(d->mutex);
    return ESP_OK;
}

static void logical_to_native(const u8g2_t *u8g2, unsigned x, unsigned y,
                               unsigned *nx, unsigned *ny)
{
    if (u8g2->cb == U8G2_R1) { *nx = WIDTH - 1U - y; *ny = x; }
    else if (u8g2->cb == U8G2_R2) { *nx = WIDTH - 1U - x; *ny = HEIGHT - 1U - y; }
    else if (u8g2->cb == U8G2_R3) { *nx = y; *ny = HEIGHT - 1U - x; }
    else { *nx = x; *ny = y; }
}

static uint8_t rgb_gray(uint16_t rgb)
{
    const unsigned r = ((rgb >> 11) & 31U) * 255U / 31U;
    const unsigned g = ((rgb >> 5) & 63U) * 255U / 63U;
    const unsigned b = (rgb & 31U) * 255U / 31U;
    return (uint8_t)(((r * 77U + g * 150U + b * 29U) * 15U + 32640U) / 65280U);
}

esp_err_t solar_os_it8951_present_frame(solar_os_it8951_t *d,
                                       const solar_os_display_raster_t *f)
{
    if (!d || !d->spi || !f || !f->data || !f->width || !f->height ||
        !f->source_width || !f->source_height) return ESP_ERR_INVALID_ARG;
    size_t row_bytes;
    switch (f->format) {
    case SOLAR_OS_DISPLAY_FORMAT_MONO1: row_bytes = (f->source_width + 7U) / 8U; break;
    case SOLAR_OS_DISPLAY_FORMAT_INDEX2: row_bytes = (f->source_width + 3U) / 4U; break;
    case SOLAR_OS_DISPLAY_FORMAT_INDEX8: row_bytes = f->source_width; break;
    case SOLAR_OS_DISPLAY_FORMAT_RGB565: row_bytes = (size_t)f->source_width * 2U; break;
    default: return ESP_ERR_NOT_SUPPORTED;
    }
    if (f->source_stride < row_bytes || f->data_size <
        (size_t)(f->source_height - 1U) * f->source_stride + row_bytes ||
        (size_t)f->x + f->width > u8g2_GetDisplayWidth(&d->u8g2) ||
        (size_t)f->y + f->height > u8g2_GetDisplayHeight(&d->u8g2)) return ESP_ERR_INVALID_SIZE;
    const bool indexed = f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ||
                         f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX2;
    if (indexed) {
        if (!f->palette_rgb565 || !f->palette_size || f->palette_size > 256U ||
            (f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 && f->palette_size > 4U) ||
            (f->clear_background && f->background_index >= f->palette_size))
            return ESP_ERR_INVALID_ARG;
        /* Validate before changing the destination, including off-screen source pixels. */
        for (unsigned y = 0; y < f->source_height; y++) {
            const uint8_t *row = f->data + (size_t)y * f->source_stride;
            for (unsigned x = 0; x < f->source_width; x++) {
                const unsigned index = f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? row[x] :
                    (row[x / 4U] >> (2U * (x & 3U))) & 3U;
                if (index >= f->palette_size) return ESP_ERR_INVALID_ARG;
            }
        }
    }
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    if (f->clear_background) {
        const uint8_t background = indexed ? rgb_gray(f->palette_rgb565[f->background_index]) :
            f->format == SOLAR_OS_DISPLAY_FORMAT_RGB565 ? rgb_gray(f->background_rgb565) :
            ((!!f->background_index != f->palette_inverted) ? 0 : 15);
        memset(d->gray, (background << 4) | background, GRAY_BYTES);
        memset(d->mono, background >= 8 ? 0xff : 0, MONO_BYTES);
    }
    for (unsigned y = 0; y < f->height; y++) {
        const unsigned sy = (uint32_t)y * f->source_height / f->height;
        const uint8_t *row = f->data + (size_t)sy * f->source_stride;
        for (unsigned x = 0; x < f->width; x++) {
            const unsigned sx = (uint32_t)x * f->source_width / f->width;
            uint8_t gray;
            if (f->format == SOLAR_OS_DISPLAY_FORMAT_MONO1) {
                const bool black = ((row[sx / 8U] >> (sx & 7U)) & 1U) != f->palette_inverted;
                gray = black ? 0 : 15;
            } else if (f->format == SOLAR_OS_DISPLAY_FORMAT_RGB565) {
                gray = rgb_gray((uint16_t)((uint16_t)row[sx * 2U] << 8) | row[sx * 2U + 1U]);
            } else {
                const unsigned index = f->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? row[sx] :
                    (row[sx / 4U] >> (2U * (sx & 3U))) & 3U;
                gray = rgb_gray(f->palette_rgb565[index]);
            }
            unsigned nx, ny;
            logical_to_native(&d->u8g2, f->x + x, f->y + y, &nx, &ny);
            set_pixel(d, nx, ny, gray);
        }
    }
    const esp_err_t err = finish_refresh(d);
    xSemaphoreGive(d->mutex);
    return err;
}

esp_err_t solar_os_it8951_present_surface(solar_os_it8951_t *d,
                                         const solar_os_display_surface_t *s)
{
    if (!s || !d || s->format != SOLAR_OS_DISPLAY_FORMAT_INDEX8 ||
        s->native_width != WIDTH || s->native_height != HEIGHT ||
        s->width != u8g2_GetDisplayWidth(&d->u8g2) ||
        s->height != u8g2_GetDisplayHeight(&d->u8g2)) return ESP_ERR_INVALID_ARG;
    const solar_os_display_raster_t f = {.data = s->data, .data_size = s->data_size,
        .palette_rgb565 = s->palette_rgb565, .palette_size = s->palette_size,
        .source_width = s->width, .source_height = s->height, .source_stride = s->stride,
        .width = s->width, .height = s->height, .format = s->format};
    return solar_os_it8951_present_frame(d, &f);
}
