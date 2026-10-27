#include "epd_ssd1685.h"
#include "epd_ssd1685_waveform.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

#define BUSY_TIMEOUT_US 30000000LL
#define TRY(call) do { esp_err_t result_ = (call); if (result_ != ESP_OK) return result_; } while (0)

static esp_err_t scan_process(void *context, const uint8_t *source,
                              bool fresh, bool *pending, bool *scanned);

static void scan_accepting(epd_ssd1685_t *d)
{
    const bool enabled = d->refresh_mode == EPD_SSD1685_REFRESH_PULSE &&
        d->scan_async && !d->suspended && !d->power_state_present &&
        !d->relax.storage && d->powered && d->controller_ready;
    epd_scan_worker_enable(&d->scan_worker, enabled);
    __atomic_store_n(&d->scan_submit_ready, enabled, __ATOMIC_RELEASE);
}

static void scan_disable(epd_ssd1685_t *d)
{
    __atomic_store_n(&d->scan_submit_ready, false, __ATOMIC_RELEASE);
    epd_scan_worker_enable(&d->scan_worker, false);
}

static void delay_ms(unsigned ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t epd_ssd1685_command(epd_ssd1685_t *d, uint8_t command,
                               const uint8_t *data, size_t length)
{
    if (!d || !d->spi || (length && !data))
        return ESP_ERR_INVALID_ARG;
    TRY(gpio_set_level(d->dc_pin, 0));
    spi_transaction_t transaction = {
        .flags = SPI_TRANS_USE_TXDATA, .length = 8, .tx_data = {command},
        .override_freq_hz = d->refresh_mode == EPD_SSD1685_REFRESH_PULSE ? d->pulse_spi_clock_hz : 0,
    };
    TRY(spi_device_polling_transmit(d->spi, &transaction));
    if (!length) return ESP_OK;
    TRY(gpio_set_level(d->dc_pin, 1));
    while (length) {
        const size_t chunk = length < EPD_SSD1685_ROW_BYTES ? length : EPD_SSD1685_ROW_BYTES;
        memcpy(d->line_buffer, data, chunk);
        transaction = (spi_transaction_t){.length = chunk * 8U, .tx_buffer = d->line_buffer,
            .override_freq_hz = d->refresh_mode == EPD_SSD1685_REFRESH_PULSE ? d->pulse_spi_clock_hz : 0};
        TRY(spi_device_polling_transmit(d->spi, &transaction));
        data += chunk;
        length -= chunk;
    }
    return ESP_OK;
}

static esp_err_t epd_ssd1685_wait_ready(epd_ssd1685_t *d)
{
    const int64_t deadline = esp_timer_get_time() + BUSY_TIMEOUT_US;
    /* BUSY is active high; allow delayed assertion after master activation. */
    delay_ms(10);
    while (gpio_get_level(d->busy_pin) != 0) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        delay_ms(20);
    }
    return ESP_OK;
}

static esp_err_t external_power(epd_ssd1685_t *d, bool on)
{
    if (d->power_pin >= 0) TRY(gpio_set_level(d->power_pin, on));
    d->powered = on;
    if (on) delay_ms(20);
    return ESP_OK;
}

/* GDEY029T71H connects sources S8..S175. Override the OTP's 168-source
 * setting with 200-source RAM addressing before selecting bytes 1..21.
 * Register definitions: Solomon Systech SSD1685 rev 0.11, commands 21/44/45.
 * Panel: https://www.good-display.com/product/540.html */
static esp_err_t set_window(epd_ssd1685_t *d)
{
    TRY(epd_ssd1685_command(d, 0x11, (uint8_t[]){3}, 1));
    TRY(epd_ssd1685_command(d, 0x44, (uint8_t[]){1, 21}, 2));
    TRY(epd_ssd1685_command(d, 0x45, (uint8_t[]){0, 0, 0x7f, 1}, 4));
    TRY(epd_ssd1685_command(d, 0x4e, (uint8_t[]){1}, 1));
    return epd_ssd1685_command(d, 0x4f, (uint8_t[]){0, 0}, 2);
}

static esp_err_t controller_init(epd_ssd1685_t *d)
{
    d->controller_ready = d->shadow_valid = d->analog_on = false;
    d->fastwave_loaded = false;
    d->pulse_needs_cleanup = true;
    d->pulse_mask_dirty = true;
    d->partial_count = 0;
    TRY(external_power(d, true));
    TRY(gpio_set_level(d->reset_pin, 1)); delay_ms(20);
    TRY(gpio_set_level(d->reset_pin, 0)); delay_ms(10);
    TRY(gpio_set_level(d->reset_pin, 1)); delay_ms(20);
    TRY(epd_ssd1685_wait_ready(d));
    TRY(epd_ssd1685_command(d, 0x12, NULL, 0));
    TRY(epd_ssd1685_wait_ready(d));
    TRY(epd_ssd1685_command(d, 0x01, (uint8_t[]){0x7f, 1, 0}, 3));
    TRY(epd_ssd1685_command(d, 0x3c, (uint8_t[]){5}, 1));
    TRY(epd_ssd1685_command(d, 0x18, (uint8_t[]){0x80}, 1));
    TRY(epd_ssd1685_command(d, 0x21, (uint8_t[]){0, 0}, 2));
    TRY(set_window(d));
    d->controller_ready = true;
    return ESP_OK;
}

/* Convert SolarOS vertical LSB-first tiles to horizontal MSB-first panel RAM.
 * Both formats use 1=white. Release the shared SPI bus before waiting on BUSY. */
static esp_err_t write_plane(epd_ssd1685_t *d, uint8_t command, const uint8_t *frame)
{
    TRY(set_window(d));
    TRY(epd_ssd1685_command(d, command, NULL, 0));
    TRY(gpio_set_level(d->dc_pin, 1));
    TRY(spi_device_acquire_bus(d->spi, portMAX_DELAY));
    esp_err_t err = ESP_OK;
    for (unsigned y = 0; y < EPD_SSD1685_HEIGHT && err == ESP_OK; ++y) {
        memset(d->line_buffer, 0, EPD_SSD1685_ROW_BYTES);
        const uint8_t *tile = frame + (y / 8U) * EPD_SSD1685_WIDTH;
        for (unsigned x = 0; x < EPD_SSD1685_WIDTH; ++x) {
            const bool white = (tile[x] & (1U << (y % 8U))) != 0;
            if (white)
                d->line_buffer[x / 8U] |= (uint8_t)(0x80U >> (x % 8U));
        }
        spi_transaction_t transaction = {
            .flags = y + 1U < EPD_SSD1685_HEIGHT ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
            .length = EPD_SSD1685_ROW_BYTES * 8U, .tx_buffer = d->line_buffer,
            .override_freq_hz = d->refresh_mode == EPD_SSD1685_REFRESH_PULSE ? d->pulse_spi_clock_hz : 0,
        };
        err = spi_device_polling_transmit(d->spi, &transaction);
    }
    if (err != ESP_OK) {
        spi_transaction_t end = {.length = 0};
        (void)spi_device_polling_transmit(d->spi, &end);
    }
    spi_device_release_bus(d->spi);
    return err;
}

static esp_err_t load_fastwave(epd_ssd1685_t *d)
{
    if (d->fastwave_loaded) return ESP_OK;
    uint8_t lut[SOLAR_OS_SSD1685_LUT_BYTES];
    if (!solar_os_ssd1685_waveform_build(lut, d->pulse_tuning.pulse_frames,
                                         d->pulse_tuning.waveform_fr, d->invert))
        return ESP_ERR_INVALID_ARG;
    /* Enable the oscillator before programming volatile waveform registers.
     * Keep the panel's OTP source/gate rails; reduce VCOM bias to legal -0.2 V.
     * No OTP programming commands or factory waveform reload bits are issued.
     */
    TRY(epd_ssd1685_command(d, 0x22, (uint8_t[]){0x80}, 1));
    TRY(epd_ssd1685_command(d, 0x20, NULL, 0));
    TRY(epd_ssd1685_wait_ready(d));
    /* SSD1685 POR strengths differ from SSD1683: only change ramp duration. */
    TRY(epd_ssd1685_command(d, 0x0c,
        (uint8_t[]){0x8b, 0x9c, 0x96, d->pulse_ramp_fast ? 0x00 : 0x0f}, 4));
    TRY(epd_ssd1685_command(d, 0x32, lut, sizeof(lut)));
    TRY(epd_ssd1685_command(d, 0x3f, (uint8_t[]){0x22}, 1));
    TRY(epd_ssd1685_command(d, 0x2c, (uint8_t[]){0x08}, 1));
    TRY(epd_ssd1685_command(d, 0x3c, (uint8_t[]){0x80}, 1));
    /* Disable factory mode-2 RAM ping-pong: both masks are written explicitly. */
    TRY(epd_ssd1685_command(d, 0x37, (uint8_t[10]){0}, 10));
    d->fastwave_loaded = true;
    return ESP_OK;
}

static esp_err_t native_frame(epd_ssd1685_t *d, const uint8_t *source, bool partial)
{
    if (!d->controller_ready || d->fastwave_loaded) {
        TRY(controller_init(d));
        partial = false;
    }
    TRY(epd_ssd1685_command(d, 0x21, (uint8_t[]){partial ? 0 : 0x40, 0}, 2));
    TRY(write_plane(d, 0x26, partial ? d->shadow : source));
    TRY(write_plane(d, 0x24, source));
    TRY(epd_ssd1685_command(d, 0x22, (uint8_t[]){partial ? 0xdc : 0xf7}, 1));
    TRY(epd_ssd1685_command(d, 0x20, NULL, 0));
    TRY(epd_ssd1685_wait_ready(d));
    d->analog_on = partial;
    TRY(write_plane(d, 0x26, source));
    if (d->shadow) {
        memcpy(d->shadow, source, EPD_SSD1685_BUFFER_BYTES);
        d->shadow_valid = true;
    }
    d->partial_count = partial ? d->partial_count + 1U : 0;
    return ESP_OK;
}

static esp_err_t pulse_powerdown(epd_ssd1685_t *d)
{
    if (!d->analog_on) return ESP_OK;
    TRY(epd_ssd1685_command(d, 0x22, (uint8_t[]){0x03}, 1));
    TRY(epd_ssd1685_command(d, 0x20, NULL, 0));
    TRY(epd_ssd1685_wait_ready(d));
    d->analog_on = false;
    return ESP_OK;
}

static void free_budget(epd_ssd1685_t *d)
{
    heap_caps_free(d->pixel_budget.active);
    heap_caps_free(d->pixel_budget.drive_history);
    heap_caps_free(d->pixel_budget.pixels);
    memset(&d->pixel_budget, 0, sizeof(d->pixel_budget));
    heap_caps_free(d->pulse_planes);
    d->pulse_planes = NULL;
}

static esp_err_t allocate_budget(epd_ssd1685_t *d)
{
    if (d->pixel_budget.pixels) return ESP_OK;
    if (!d->shadow) return ESP_ERR_NO_MEM;
    const size_t count = EPD_SSD1685_WIDTH * EPD_SSD1685_HEIGHT;
    const unsigned caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    uint8_t *pixels = heap_caps_malloc(count, caps);
    uint32_t *active = heap_caps_malloc(count * sizeof(*active), caps);
    uint16_t *history = heap_caps_malloc(count * sizeof(*history), caps);
    d->pulse_planes = heap_caps_malloc(EPD_SSD1685_BUFFER_BYTES * 2U, caps);
    if (!pixels || !active || !history || !d->pulse_planes) {
        heap_caps_free(pixels); heap_caps_free(active); heap_caps_free(history);
        heap_caps_free(d->pulse_planes); d->pulse_planes = NULL;
        return ESP_ERR_NO_MEM;
    }
    TRY(epd_pixel_budget_init(&d->pixel_budget, pixels, count));
    TRY(epd_pixel_budget_set_active_storage(&d->pixel_budget, active, count));
    TRY(epd_pixel_budget_set_drive_history(&d->pixel_budget, history, count));
    d->pulse_needs_cleanup = true;
    return ESP_OK;
}

static bool pixel_white(const uint8_t *source, size_t p)
{
    const size_t x = p % EPD_SSD1685_WIDTH, y = p / EPD_SSD1685_WIDTH;
    return (source[(y / 8U) * EPD_SSD1685_WIDTH + x] & (1U << (y % 8U))) != 0;
}

static bool cleanup_due(const epd_ssd1685_t *d)
{
    const epd_pulse_tuning_t *t = &d->pulse_tuning;
    return d->pulse_updates >= t->cleanup_every * (t->cleanup_adaptive ? 4U : 1U) ||
        (t->cleanup_adaptive && d->pixel_budget.peak_drive_cycles >= t->cleanup_every * t->drive_cycles);
}

static esp_err_t pulse_prepare(epd_ssd1685_t *d, const uint8_t *source,
                                bool fresh, bool *baseline)
{
    *baseline = false;
    TRY(allocate_budget(d));
    if (!d->controller_ready || !d->shadow_valid || d->pulse_needs_cleanup || cleanup_due(d)) {
        TRY(native_frame(d, source, false));
        epd_pixel_budget_reset(&d->pixel_budget);
        for (size_t p = 0; p < d->pixel_budget.count; ++p)
            epd_pixel_budget_seed(&d->pixel_budget, p, pixel_white(source, p));
        d->pulse_needs_cleanup = false;
        d->pulse_mask_dirty = true;
        d->pulse_updates = 0;
        d->pulse_last_frame_start = 0;
        *baseline = true;
        return ESP_OK;
    }
    if (fresh && memcmp(source, d->shadow, EPD_SSD1685_BUFFER_BYTES) != 0) {
        if (!d->scan_async && d->pulse_last_frame_start) {
            const int64_t due = d->pulse_last_frame_start + (int64_t)d->scan_period_ms * 1000;
            while (esp_timer_get_time() < due) delay_ms(1);
        }
        d->pulse_last_frame_start = esp_timer_get_time();
        for (size_t p = 0; p < d->pixel_budget.count; ++p)
            if (pixel_white(source, p) != pixel_white(d->shadow, p))
                epd_pixel_budget_target(&d->pixel_budget, p, pixel_white(source, p), d->pulse_tuning.drive_cycles);
        memcpy(d->shadow, source, EPD_SSD1685_BUFFER_BYTES);
        ++d->pulse_updates; /* changed targets, never physical settling scans */
        d->pulse_mask_dirty = true;
    }
    return ESP_OK;
}

static esp_err_t pulse_cycle(epd_ssd1685_t *d)
{
    TRY(load_fastwave(d));
    if (d->pulse_mask_dirty || d->pulse_mask_pending != d->pixel_budget.pending) {
        uint8_t *hold = d->pulse_planes, *white = hold + EPD_SSD1685_BUFFER_BYTES;
        memset(hold, 0xff, EPD_SSD1685_BUFFER_BYTES);
        memset(white, 0, EPD_SSD1685_BUFFER_BYTES);
        for (size_t i = 0; i < d->pixel_budget.active_count; ++i) {
            const size_t p = d->pixel_budget.active[i];
            const epd_pixel_drive_t drive = epd_pixel_budget_drive(&d->pixel_budget, p);
            if (drive == EPD_PIXEL_HOLD) continue;
            const size_t x = p % EPD_SSD1685_WIDTH, y = p / EPD_SSD1685_WIDTH;
            const size_t offset = (y / 8U) * EPD_SSD1685_WIDTH + x;
            const uint8_t bit = (uint8_t)(1U << (y % 8U));
            hold[offset] &= (uint8_t)~bit;
            if (drive == EPD_PIXEL_DRIVE_WHITE) white[offset] |= bit;
        }
        /* Mode 1: (RAM26,RAM24) hold=(1,0), white=(0,1), black=(0,0). */
        TRY(epd_ssd1685_command(d, 0x21, (uint8_t[]){0, 0}, 2));
        TRY(write_plane(d, 0x26, hold));
        TRY(write_plane(d, 0x24, white));
        d->pulse_mask_dirty = false;
        d->pulse_mask_pending = d->pixel_budget.pending;
    }
    const uint8_t update = d->pulse_power_hold ? (d->analog_on ? 0x04 : 0xc4) : 0xc7;
    TRY(epd_ssd1685_command(d, 0x22, &update, 1));
    TRY(epd_ssd1685_command(d, 0x20, NULL, 0));
    d->analog_on = d->pulse_power_hold;
    const int64_t assert_deadline = esp_timer_get_time() + 50000;
    while (!gpio_get_level(d->busy_pin)) {
        if (esp_timer_get_time() >= assert_deadline) return ESP_ERR_TIMEOUT;
        delay_ms(1);
    }
    TRY(epd_ssd1685_wait_ready(d));
    epd_pixel_budget_advance(&d->pixel_budget); /* only after BUSY completes */
    return ESP_OK;
}

static void pulse_failed(epd_ssd1685_t *d)
{
    scan_disable(d);
    d->refresh_mode = EPD_SSD1685_REFRESH_AUTO;
    d->controller_ready = d->shadow_valid = false;
    d->pulse_needs_cleanup = true;
}

static esp_err_t scan_process(void *context, const uint8_t *source,
                              bool fresh, bool *pending, bool *scanned)
{
    epd_ssd1685_t *d = context;
    *pending = *scanned = false;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (__atomic_load_n(&d->scan_submit_ready, __ATOMIC_ACQUIRE) && !d->suspended &&
        d->refresh_mode == EPD_SSD1685_REFRESH_PULSE && d->scan_async) {
        bool baseline;
        err = pulse_prepare(d, source, fresh, &baseline);
        if (err == ESP_OK && !baseline && d->pixel_budget.pending) {
            err = pulse_cycle(d);
            *scanned = err == ESP_OK;
        }
        if (err == ESP_OK && !d->pixel_budget.pending) err = pulse_powerdown(d);
        if (err != ESP_OK) {
            pulse_failed(d);
            /* Recover the newest accepted image even if the app stops painting. */
            xSemaphoreTake(d->scan_worker.lock, portMAX_DELAY);
            memcpy(d->scan_worker.snapshot, d->scan_worker.latest, EPD_SSD1685_BUFFER_BYTES);
            xSemaphoreGive(d->scan_worker.lock);
            if (native_frame(d, d->scan_worker.snapshot, false) != ESP_OK)
                d->controller_ready = d->shadow_valid = false;
        } else *pending = d->pixel_budget.pending != 0 || cleanup_due(d);
        d->last_error = err;
    }
    if (d->lock) xSemaphoreGive(d->lock);
    return err;
}

static esp_err_t refresh_frame(epd_ssd1685_t *d)
{
    if (d->suspended) return ESP_ERR_INVALID_STATE;
    if (d->refresh_mode == EPD_SSD1685_REFRESH_PULSE) {
        bool baseline;
        TRY(pulse_prepare(d, d->buffer, true, &baseline));
        if (!baseline) while (d->pixel_budget.pending) TRY(pulse_cycle(d));
        TRY(pulse_powerdown(d));
        if (cleanup_due(d)) TRY(pulse_prepare(d, d->buffer, false, &baseline));
        return ESP_OK;
    }
    const bool baseline = d->controller_ready && d->shadow && d->shadow_valid && !d->fastwave_loaded;
    if (baseline && d->refresh_mode != EPD_SSD1685_REFRESH_FULL &&
        memcmp(d->shadow, d->buffer, EPD_SSD1685_BUFFER_BYTES) == 0) return ESP_OK;
    const bool partial = baseline && d->refresh_mode != EPD_SSD1685_REFRESH_FULL &&
        (d->refresh_mode == EPD_SSD1685_REFRESH_PARTIAL || d->partial_count < d->pulse_tuning.cleanup_every);
    return native_frame(d, d->buffer, partial);
}

esp_err_t epd_ssd1685_present_power_state(epd_ssd1685_t *d)
{
    if (!d || !d->spi || !d->buffer || !d->line_buffer || !d->lock)
        return ESP_ERR_INVALID_STATE;
    scan_disable(d);
    xSemaphoreTake(d->lock, portMAX_DELAY);
    d->power_state_present = true;
    /* Join the current scan, then discard any target it re-enabled. A full
     * native refresh must finish before suspend or system power-off. */
    scan_disable(d);
    const esp_err_t err = d->suspended ? ESP_ERR_INVALID_STATE :
        native_frame(d, d->buffer, false);
    if (err != ESP_OK) d->controller_ready = d->shadow_valid = false;
    d->last_error = err;
    xSemaphoreGive(d->lock);
    return err;
}

esp_err_t epd_ssd1685_refresh(epd_ssd1685_t *d)
{
    if (!d) return ESP_ERR_INVALID_STATE;
    if (__atomic_load_n(&d->scan_submit_ready, __ATOMIC_ACQUIRE) &&
        epd_scan_worker_submit(&d->scan_worker, d->buffer) == ESP_OK) return ESP_OK;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    if (d->suspended || !d->spi || !d->buffer || !d->line_buffer) {
        if (d->lock) xSemaphoreGive(d->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (d->relax.storage) {
        if (d->lock) xSemaphoreGive(d->lock);
        return ESP_OK;
    }
    esp_err_t err = refresh_frame(d);
    if (err != ESP_OK) {
        if (d->refresh_mode == EPD_SSD1685_REFRESH_PULSE) pulse_failed(d);
        else d->controller_ready = d->shadow_valid = false;
    }
    d->last_error = err;
    if (err == ESP_OK) scan_accepting(d);
    if (d->lock) xSemaphoreGive(d->lock);
    return err;
}

esp_err_t epd_ssd1685_present_mono_xbm(epd_ssd1685_t *d,
                                       const uint8_t *bitmap, size_t bitmap_size,
                                       uint16_t x, uint16_t y, uint16_t width,
                                       uint16_t height, uint16_t stride, bool inverted)
{
    if (!d || !d->buffer || !bitmap || !width || !height ||
        stride < ((size_t)width + 7U) / 8U || height > SIZE_MAX / stride ||
        bitmap_size < (size_t)height * stride ||
        (uint32_t)x + width > u8g2_GetDisplayWidth(&d->u8g2) ||
        (uint32_t)y + height > u8g2_GetDisplayHeight(&d->u8g2)) return ESP_ERR_INVALID_ARG;
    const u8g2_cb_t *rotation = d->u8g2.cb;
    if (rotation != U8G2_R0 && rotation != U8G2_R1 &&
        rotation != U8G2_R2 && rotation != U8G2_R3) return ESP_ERR_NOT_SUPPORTED;
    if (!d->powered || d->suspended || d->relax.storage || !d->spi || !d->line_buffer)
        return ESP_ERR_INVALID_STATE;
    /* Like ordinary u8g2 drawing, compose before submitting the snapshot.
     * The worker reads its own copy, never this buffer. Async submission must
     * not wait on the panel lock held during an earlier physical drive.
     */
    memset(d->buffer, inverted ? 0 : 0xff, EPD_SSD1685_BUFFER_BYTES);
    for (unsigned row = 0; row < height; ++row) {
        const uint8_t *source = bitmap + (size_t)row * stride;
        for (unsigned column = 0; column < width; ++column) {
            const unsigned lx = x + column, ly = y + row;
            unsigned nx = lx, ny = ly;
            if (rotation == U8G2_R1) { nx = EPD_SSD1685_WIDTH - 1U - ly; ny = lx; }
            else if (rotation == U8G2_R2) {
                nx = EPD_SSD1685_WIDTH - 1U - lx; ny = EPD_SSD1685_HEIGHT - 1U - ly;
            } else if (rotation == U8G2_R3) { nx = ly; ny = EPD_SSD1685_HEIGHT - 1U - lx; }
            uint8_t *destination = d->buffer + (ny / 8U) * EPD_SSD1685_WIDTH + nx;
            const uint8_t mask = (uint8_t)(1U << (ny & 7U));
            const bool black = (source[column / 8U] & (1U << (column & 7U))) != 0;
            if (black == inverted) *destination |= mask;
            else *destination &= (uint8_t)~mask;
        }
    }
    return epd_ssd1685_refresh(d);
}

static esp_err_t sleep_panel(epd_ssd1685_t *d)
{
    d->suspended = true;
    scan_disable(d);
    esp_err_t err = ESP_OK;
    if (d->powered) {
        if (d->analog_on) {
            err = epd_ssd1685_command(d, 0x22, (uint8_t[]){0x83}, 1);
            if (err == ESP_OK) err = epd_ssd1685_command(d, 0x20, NULL, 0);
            if (err == ESP_OK) err = epd_ssd1685_wait_ready(d);
        }
        if (err == ESP_OK) err = epd_ssd1685_command(d, 0x10, (uint8_t[]){1}, 1);
    }
    const esp_err_t power_err = external_power(d, false);
    d->controller_ready = d->shadow_valid = d->analog_on = false;
    return err != ESP_OK ? err : power_err;
}

static const u8x8_display_info_t display_info = {
    .chip_enable_level = 0, .chip_disable_level = 1,
    .reset_pulse_width_ms = 10, .post_reset_wait_ms = 50,
    .sck_clock_hz = 10000000, .spi_mode = 0,
    .tile_width = EPD_SSD1685_WIDTH / 8U,
    .tile_height = EPD_SSD1685_HEIGHT / 8U,
    .pixel_width = EPD_SSD1685_WIDTH, .pixel_height = EPD_SSD1685_HEIGHT,
};

static uint8_t display_cb(u8x8_t *u8x8, uint8_t message, uint8_t arg, void *ptr)
{
    (void)ptr;
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &display_info);
        return 1;
    }
    epd_ssd1685_t *d = (epd_ssd1685_t *)((uint8_t *)u8x8 -
        offsetof(epd_ssd1685_t, u8g2) - offsetof(u8g2_t, u8x8));
    esp_err_t err;
    switch (message) {
    case U8X8_MSG_DISPLAY_INIT:
        if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
        err = controller_init(d);
        if (d->lock) xSemaphoreGive(d->lock);
        break;
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
        if (arg) err = sleep_panel(d);
        else {
            d->suspended = false;
            d->power_state_present = false;
            err = d->controller_ready ? ESP_OK : controller_init(d);
            if (err == ESP_OK) scan_accepting(d);
        }
        if (d->lock) xSemaphoreGive(d->lock);
        break;
    case U8X8_MSG_DISPLAY_REFRESH: err = epd_ssd1685_refresh(d); break;
    case U8X8_MSG_DISPLAY_DRAW_TILE: return 1;
    default: return 0;
    }
    d->last_error = err;
    return err == ESP_OK;
}

static bool config_valid(const epd_ssd1685_config_t *c)
{
    if (!c || !c->spi_bus || !c->spi_bus[0] || !c->rotation ||
        c->spi_clock_hz < 100000 || c->spi_clock_hz > 10000000 || c->power_pin < -1 ||
        c->panel != EPD_SSD1685_PANEL_GDEY029T71H)
        return false;
    const int pins[] = {c->cs_pin, c->dc_pin, c->reset_pin, c->busy_pin, c->power_pin};
    for (unsigned i = 0; i < 5; ++i) {
        if (i == 4 && pins[i] == -1) continue;
        if (pins[i] < 0 || pins[i] >= GPIO_NUM_MAX || pins[i] >= 64) return false;
        for (unsigned j = i + 1; j < 5; ++j) if (pins[i] == pins[j]) return false;
    }
    return true;
}

static esp_err_t configure_pins(epd_ssd1685_t *d)
{
    const gpio_config_t output = {
        .pin_bit_mask = (1ULL << d->dc_pin) | (1ULL << d->reset_pin) |
            (d->power_pin >= 0 ? 1ULL << d->power_pin : 0),
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    d->pins_configured = true;
    TRY(gpio_config(&output));
    const gpio_config_t input = {
        .pin_bit_mask = 1ULL << d->busy_pin, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    TRY(gpio_config(&input));
    TRY(gpio_set_level(d->dc_pin, 1));
    return gpio_set_level(d->reset_pin, 1);
}

esp_err_t epd_ssd1685_init(epd_ssd1685_t *d, const epd_ssd1685_config_t *c)
{
    if (!d || !config_valid(c))
        return ESP_ERR_INVALID_ARG;
    *d = (epd_ssd1685_t){.dc_pin = c->dc_pin,
        .reset_pin = c->reset_pin, .busy_pin = c->busy_pin, .power_pin = c->power_pin,
        .pulse_tuning = {.drive_cycles = 3, .pulse_frames = 10, .waveform_fr = 5,
                         .cleanup_every = 64, .cleanup_adaptive = true},
        .scan_async = true, .scan_period_ms = 100, .pulse_power_hold = true,
        .pulse_spi_clock_hz = c->spi_clock_hz};
    d->lock = xSemaphoreCreateMutexStatic(&d->lock_storage);
    esp_err_t err = configure_pins(d);
    if (err == ESP_OK) err = external_power(d, false);
    if (err == ESP_OK) {
        const spi_device_interface_config_t device_config = {
            .clock_speed_hz = c->spi_clock_hz, .mode = 0,
            .spics_io_num = c->cs_pin, .queue_size = 1,
        };
        err = solar_os_bus_spi_add_device(c->spi_bus, &device_config, &d->spi);
    }
    if (err == ESP_OK) {
        d->line_buffer = heap_caps_malloc(EPD_SSD1685_ROW_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        d->buffer = heap_caps_calloc(1, EPD_SSD1685_BUFFER_BYTES, MALLOC_CAP_8BIT);
        d->shadow = heap_caps_malloc(EPD_SSD1685_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!d->shadow) d->shadow = heap_caps_malloc(EPD_SSD1685_BUFFER_BYTES, MALLOC_CAP_8BIT);
        if (!d->line_buffer || !d->buffer) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        memset(d->buffer, 0xff, EPD_SSD1685_BUFFER_BYTES);
        u8g2_SetupDisplay(&d->u8g2, display_cb, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
        u8g2_SetupBuffer(&d->u8g2, d->buffer, EPD_SSD1685_HEIGHT / 8U,
                         u8g2_ll_hvline_vertical_top_lsb, c->rotation);
        u8g2_InitDisplay(&d->u8g2);
        err = d->last_error;
    }
    if (err == ESP_OK)
        err = epd_ssd1685_set_controller_mode(d, "refresh=pulse");
    if (err != ESP_OK) epd_ssd1685_deinit(d);
    return err;
}

esp_err_t epd_ssd1685_resume(epd_ssd1685_t *d)
{
    if (!d || !d->spi || !d->buffer) return ESP_ERR_INVALID_STATE;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    d->suspended = false;
    d->power_state_present = false;
    esp_err_t err = configure_pins(d);
    if (err == ESP_OK) err = controller_init(d);
    d->last_error = err;
    if (err == ESP_OK) scan_accepting(d);
    if (d->lock) xSemaphoreGive(d->lock);
    return err;
}

void epd_ssd1685_deinit(epd_ssd1685_t *d)
{
    if (!d) return;
    scan_disable(d);
    /* Join outside the panel lock: an in-flight callback may be waiting on it. */
    epd_scan_worker_deinit(&d->scan_worker);
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    if (d->spi && d->powered) (void)sleep_panel(d);
    if (d->spi) (void)spi_bus_remove_device(d->spi);
    heap_caps_free(d->line_buffer); heap_caps_free(d->buffer); heap_caps_free(d->shadow);
    free_budget(d);
    epd_relax_dispose(&d->relax);
    if (d->pins_configured) {
        (void)gpio_reset_pin(d->dc_pin); (void)gpio_reset_pin(d->reset_pin);
        (void)gpio_reset_pin(d->busy_pin);
        if (d->power_pin >= 0) (void)gpio_reset_pin(d->power_pin);
    }
    if (d->lock) xSemaphoreGive(d->lock);
    memset(d, 0, sizeof(*d));
}

u8g2_t *epd_ssd1685_get_u8g2(epd_ssd1685_t *d) { return d ? &d->u8g2 : NULL; }

const char *epd_ssd1685_controller_mode(const epd_ssd1685_t *d)
{
    if (!d) return NULL;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    const char *refresh = d->refresh_mode == EPD_SSD1685_REFRESH_PARTIAL ? "partial" :
        d->refresh_mode == EPD_SSD1685_REFRESH_FULL ? "full" :
        d->refresh_mode == EPD_SSD1685_REFRESH_PULSE ? "pulse" : "auto";
    /* Same per-instance description pattern as the other e-paper drivers. */
    char *description = ((epd_ssd1685_t *)d)->mode_description;
    const epd_scan_stats_t stats = epd_scan_worker_stats(&((epd_ssd1685_t *)d)->scan_worker);
    snprintf(description, sizeof(d->mode_description),
             "refresh=%s waveform=%s drive-cycles=%u pulse-frames=%u waveform-fr=%u "
             "cleanup-every=%u cleanup-policy=%s cleanup-peak-cycles=%u cleanup-limit-cycles=%u cleanup-age=%u "
             "pulse-scan=%s pulse-period=%u pulse-power=%s pulse-ramp=%s pulse-spi-khz=%u invert=%s "
             "scan-last-us=%lld scan-interval-us=%lld scan-count=%u frame-count=%u frame-interval-us=%lld "
             "scan-submitted=%u scan-coalesced=%u pending=%u",
             refresh, d->refresh_mode == EPD_SSD1685_REFRESH_PULSE ? "fastwave" : "otp",
             d->pulse_tuning.drive_cycles, d->pulse_tuning.pulse_frames, d->pulse_tuning.waveform_fr,
             d->pulse_tuning.cleanup_every, d->pulse_tuning.cleanup_adaptive ? "adaptive" : "fixed",
             (unsigned)d->pixel_budget.peak_drive_cycles,
             d->pulse_tuning.cleanup_every * d->pulse_tuning.drive_cycles, (unsigned)d->pulse_updates,
             d->scan_async ? "async" : "sync", d->scan_period_ms,
             d->pulse_power_hold ? "hold" : "cycle", d->pulse_ramp_fast ? "fast" : "normal",
             (unsigned)(d->pulse_spi_clock_hz / 1000), d->invert ? "true" : "false",
             (long long)stats.last_scan_us, (long long)stats.last_interval_us, (unsigned)stats.scans,
             (unsigned)stats.frames, (long long)stats.last_frame_interval_us,
             (unsigned)stats.submitted, (unsigned)stats.coalesced, (unsigned)d->pixel_budget.pending);
    if (d->lock) xSemaphoreGive(d->lock);
    return description;
}

const char *epd_ssd1685_controller_mode_values(const epd_ssd1685_t *d)
{
    (void)d;
    return "refresh=<auto,fast,partial,full,pulse> waveform=<otp,fastwave> "
           "drive-cycles=<1..32> pulse-frames=<1..32> waveform-fr=<1..12> "
           "cleanup-every=<1..128> cleanup-policy=<adaptive,fixed> "
           "pulse-scan=<sync,async> pulse-period=<67..1000> pulse-power=<cycle,hold> "
           "pulse-ramp=<normal,fast> pulse-spi-khz=<100..10000> invert=<false,true>";
}

static esp_err_t relax_native(void *context, solar_os_display_relax_phase_t phase,
                               const uint8_t *image)
{
    epd_ssd1685_t *d = context;
    if (phase == SOLAR_OS_DISPLAY_RELAX_BEGIN) {
        scan_disable(d);
        esp_err_t err = pulse_powerdown(d);
        if (err == ESP_OK) err = controller_init(d);
        if (err != ESP_OK) d->controller_ready = d->shadow_valid = false;
        return err;
    }
    const esp_err_t err = !d->powered || d->suspended ? ESP_ERR_INVALID_STATE : native_frame(d, image, false);
    if (err != ESP_OK) d->controller_ready = d->shadow_valid = false;
    if (phase == SOLAR_OS_DISPLAY_RELAX_END) {
        if (err == ESP_OK && d->pixel_budget.pixels) {
            epd_pixel_budget_reset(&d->pixel_budget);
            for (size_t p = 0; p < d->pixel_budget.count; ++p)
                epd_pixel_budget_seed(&d->pixel_budget, p, pixel_white(image, p));
        }
        d->pulse_updates = 0;
        d->pulse_needs_cleanup = err != ESP_OK;
        d->pulse_mask_dirty = true;
        d->pulse_last_frame_start = 0;
    }
    return err;
}

esp_err_t epd_ssd1685_relax(epd_ssd1685_t *d, solar_os_display_relax_phase_t phase)
{
    if (!d || !d->buffer || (unsigned)phase > SOLAR_OS_DISPLAY_RELAX_END) return ESP_ERR_INVALID_ARG;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err = phase == SOLAR_OS_DISPLAY_RELAX_BEGIN && (!d->powered || d->suspended) ? ESP_ERR_INVALID_STATE :
        epd_relax_apply(&d->relax, d->buffer, EPD_SSD1685_BUFFER_BYTES, phase, relax_native, d);
    d->last_error = err;
    scan_accepting(d);
    if (d->lock) xSemaphoreGive(d->lock);
    return err;
}

static bool parse_tuning(const char *text, unsigned maximum, unsigned *value)
{
    if (*text < '0' || *text > '9') return false;
    char *end;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (*end || parsed < 1 || parsed > maximum) return false;
    *value = (unsigned)parsed;
    return true;
}

static esp_err_t set_controller_mode(epd_ssd1685_t *d, const char *mode)
{
    if (d->relax.storage) return ESP_ERR_INVALID_STATE;
    const epd_ssd1685_refresh_mode_t before = d->refresh_mode;
    bool restart = false;
    if (strcmp(mode, "refresh=auto") == 0 || strcmp(mode, "waveform=otp") == 0)
        d->refresh_mode = EPD_SSD1685_REFRESH_AUTO;
    else if (strcmp(mode, "refresh=partial") == 0 || strcmp(mode, "refresh=fast") == 0)
        d->refresh_mode = EPD_SSD1685_REFRESH_PARTIAL;
    else if (strcmp(mode, "refresh=full") == 0) d->refresh_mode = EPD_SSD1685_REFRESH_FULL;
    else if (strcmp(mode, "refresh=pulse") == 0 || strcmp(mode, "waveform=fastwave") == 0) {
        const bool allocated = d->pixel_budget.pixels != NULL;
        TRY(allocate_budget(d));
        if (d->scan_async && !d->scan_worker.task) {
            const esp_err_t err = epd_scan_worker_init(&d->scan_worker, EPD_SSD1685_BUFFER_BYTES, scan_process, d);
            if (err != ESP_OK) {
                if (!allocated) free_budget(d);
                return err;
            }
            TRY(epd_scan_worker_set_period(&d->scan_worker, d->scan_period_ms));
        }
        d->refresh_mode = EPD_SSD1685_REFRESH_PULSE;
        restart = before != d->refresh_mode;
    } else if (strcmp(mode, "pulse-scan=async") == 0) {
        if (!d->scan_worker.task) {
            TRY(epd_scan_worker_init(&d->scan_worker, EPD_SSD1685_BUFFER_BYTES, scan_process, d));
            TRY(epd_scan_worker_set_period(&d->scan_worker, d->scan_period_ms));
        }
        restart = !d->scan_async;
        d->scan_async = true;
    } else if (strcmp(mode, "pulse-scan=sync") == 0) {
        restart = d->scan_async;
        d->scan_async = false;
    } else if (strcmp(mode, "pulse-power=hold") == 0 || strcmp(mode, "pulse-power=cycle") == 0) {
        d->pulse_power_hold = strcmp(mode, "pulse-power=hold") == 0;
        restart = true;
    } else if (strcmp(mode, "pulse-ramp=fast") == 0 || strcmp(mode, "pulse-ramp=normal") == 0) {
        d->pulse_ramp_fast = strcmp(mode, "pulse-ramp=fast") == 0;
        restart = true;
    } else if (strncmp(mode, "pulse-period=", 13) == 0 || strncmp(mode, "pulse-spi-khz=", 14) == 0) {
        const bool spi = mode[6] == 's';
        unsigned value;
        if (!parse_tuning(mode + (spi ? 14 : 13), spi ? 10000 : 1000, &value) ||
            value < (spi ? 100U : 67U)) return ESP_ERR_INVALID_ARG;
        if (spi) d->pulse_spi_clock_hz = value * 1000U;
        else {
            if (d->scan_worker.task) TRY(epd_scan_worker_set_period(&d->scan_worker, value));
            d->scan_period_ms = (uint16_t)value;
        }
    } else if (strcmp(mode, "invert=true") == 0 || strcmp(mode, "invert=false") == 0) {
        d->invert = strcmp(mode, "invert=true") == 0;
        restart = true;
    } else {
        const epd_pulse_tuning_t previous = d->pulse_tuning;
        TRY(epd_pulse_tuning_set(&d->pulse_tuning, mode));
        restart = previous.pulse_frames != d->pulse_tuning.pulse_frames ||
            previous.waveform_fr != d->pulse_tuning.waveform_fr;
    }
    if (restart || (before == EPD_SSD1685_REFRESH_PULSE && before != d->refresh_mode))
        d->pulse_needs_cleanup = true;
    if (before == EPD_SSD1685_REFRESH_PULSE && d->refresh_mode != before) {
        d->controller_ready = d->shadow_valid = false;
        free_budget(d);
    }
    scan_accepting(d);
    if (d->refresh_mode != EPD_SSD1685_REFRESH_PULSE || !d->scan_async)
        TRY(pulse_powerdown(d));
    return ESP_OK;
}

esp_err_t epd_ssd1685_set_controller_mode(epd_ssd1685_t *d, const char *mode)
{
    if (!d || !mode) return ESP_ERR_INVALID_ARG;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    const esp_err_t err = set_controller_mode(d, mode);
    if (d->lock) xSemaphoreGive(d->lock);
    return err;
}
