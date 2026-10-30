/* Sticky 3D97 V2 monochrome panel protocol:
 * Seeed-Studio/Seeed_GFX2 834c8cafa108805f2b625732998b86b31d683da1,
 * src/driver/epaper/Driver_SSD2677.cpp. Register values describe this panel,
 * not generic SSD2677 color glass. No SSD1677 LUT/registers are used here.
 * See doc/licenses/seeed-ssd2677-notice.txt.
 */
#include "epd_ssd2677.h"
#include "epd_ssd2677_waveform.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "solar_os_memory.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

#define SSD2677_WIDTH 800U
#define SSD2677_HEIGHT 480U
#define SSD2677_BUFFER_BYTES (SSD2677_WIDTH * SSD2677_HEIGHT / 8U)
#define SSD2677_ROW_BYTES (SSD2677_WIDTH / 4U)
#define SSD2677_TRANSFER_BYTES (SSD2677_ROW_BYTES * 8U)
#define SSD2677_TEMPERATURE_INTERVAL_US INT64_C(5000000)
#define TRY(call) do { esp_err_t err_ = (call); if (err_ != ESP_OK) return err_; } while (0)

static uint8_t reverse_byte(uint8_t value);
static void transpose_block(uint32_t *low, uint32_t *high);
static esp_err_t scan_process(void *context, const uint8_t *source,
                              bool fresh, bool *pending, bool *scanned);
static void scan_accepting(epd_ssd2677_t *d);
static void scan_disable(epd_ssd2677_t *d);
static void free_budget(epd_ssd2677_t *d);
static void pulse_failed(epd_ssd2677_t *d);
static esp_err_t refresh_frame(epd_ssd2677_t *d);

static uint32_t write_clock(const epd_ssd2677_t *d)
{
    return d->fastwave_loaded ? d->pulse_spi_clock_hz : d->spi_clock_hz;
}

static void delay_ms(unsigned ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t transmit(epd_ssd2677_t *d, spi_transaction_t *transaction, uint32_t hz)
{
    /* ESP-IDF frequency overrides persist for the device; zero keeps the
     * previous rate. Restore the configured write rate after a slow read. */
    transaction->override_freq_hz = d->spi_applied_hz == hz ? 0 : hz;
    const esp_err_t err = spi_device_polling_transmit(d->spi, transaction);
    d->spi_applied_hz = err == ESP_OK ? hz : 0;
    return err;
}

static esp_err_t command(epd_ssd2677_t *d, uint8_t cmd, const uint8_t *data, size_t size)
{
    TRY(gpio_set_level(d->dc_pin, 0));
    spi_transaction_t tx = {.flags = SPI_TRANS_USE_TXDATA, .length = 8, .tx_data = {cmd}};
    TRY(transmit(d, &tx, write_clock(d)));
    if (size) {
        /* All command parameters fit the internal DMA staging buffer. */
        if (!data || size > d->line_buffer_size) return ESP_ERR_INVALID_ARG;
        memcpy(d->line_buffer, data, size);
        TRY(gpio_set_level(d->dc_pin, 1));
        tx = (spi_transaction_t){.length = size * 8U, .tx_buffer = d->line_buffer};
        TRY(transmit(d, &tx, write_clock(d)));
    }
    return ESP_OK;
}

static esp_err_t wait_ready(epd_ssd2677_t *d)
{
    const int64_t deadline = esp_timer_get_time() + 30000000;
    delay_ms(1); /* Allow BUSY to assert after a command. BUSY is active LOW. */
    while (!gpio_get_level(d->busy_pin)) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        delay_ms(2);
    }
    return ESP_OK;
}

static esp_err_t set_power(epd_ssd2677_t *d, bool on)
{
    if (d->set_power) TRY(d->set_power(d->power_context, on));
    if (d->power_pin >= 0)
        TRY(gpio_set_level(d->power_pin, on ? d->power_active_level : !d->power_active_level));
    d->powered = on;
    if (on) delay_ms(20);
    return ESP_OK;
}

static esp_err_t powerdown(epd_ssd2677_t *d)
{
    if (!d->analog_on) return ESP_OK;
    TRY(command(d, 0x02, (uint8_t[]){0}, 1));
    TRY(wait_ready(d));
    d->analog_on = false;
    return ESP_OK;
}

static esp_err_t controller_init(epd_ssd2677_t *d)
{
    d->controller_ready = d->shadow_valid = d->analog_on = false;
    d->temperature_valid = d->waveform_valid = false;
    d->fastwave_loaded = false;
    d->pulse_window_valid = false;
    TRY(set_power(d, true));
    TRY(gpio_set_level(d->reset_pin, 0));
    delay_ms(20);
    TRY(gpio_set_level(d->reset_pin, 1));
    delay_ms(50);
    TRY(wait_ready(d));
    TRY(command(d, 0x00, (uint8_t[]){0x2f, 0x0e}, 2));
    TRY(wait_ready(d));
    TRY(command(d, 0x06, (uint8_t[]){0x0f, 0x8b, 0x93, 0xc1}, 4));
    TRY(command(d, 0xe7, (uint8_t[]){0xc1}, 1));
    TRY(command(d, 0x30, (uint8_t[]){0x08}, 1));
    TRY(command(d, 0x50, (uint8_t[]){0x77}, 1));
    TRY(command(d, 0x62, (uint8_t[]){0x76,0x76,0x76,0x5a,0x9d,0x8a,0x76,0x62}, 8));
    /* 680 gates are scanned; only 480 rows of image data are visible. */
    TRY(command(d, 0x61, (uint8_t[]){0x03,0x20,0x02,0xa8}, 4));
    TRY(command(d, 0xe0, (uint8_t[]){0x10}, 1));
    TRY(command(d, 0x65, (uint8_t[4]){0}, 4));
    TRY(command(d, 0xe9, (uint8_t[]){0x01}, 1));
    TRY(wait_ready(d));
    d->controller_ready = true;
    d->partial_refresh_count = 0;
    return ESP_OK;
}

static esp_err_t latch_waveform(epd_ssd2677_t *d, bool partial)
{
    const int64_t now = esp_timer_get_time();
    if (!partial || !d->temperature_valid || now - d->temperature_read_us >= SSD2677_TEMPERATURE_INTERVAL_US) {
        TRY(command(d, 0x40, NULL, 0));
        TRY(wait_ready(d));
        uint8_t temperature = 0;
        TRY(gpio_set_level(d->dc_pin, 1));
        spi_transaction_t rx = {.rxlength = 8, .rx_buffer = &temperature};
        const uint32_t read_hz = d->spi_clock_hz < 1000000 ? d->spi_clock_hz : 1000000;
        TRY(transmit(d, &rx, read_hz));
        /* Some panels have no working readback. Match Seeed's 25 C fallback. */
        d->temperature = temperature && temperature <= 127 ? temperature : 25;
        d->temperature_read_us = esp_timer_get_time();
        d->temperature_valid = true;
    }
    const unsigned bucket = d->temperature <= 10 ? 0 : d->temperature <= 20 ? 1 :
        d->temperature <= 30 ? 2 : 3;
    static const uint8_t full[] = {0xe8,0xeb,0xee,0xf1};
    static const uint8_t fast[] = {0x08,0x0f,0x19,0x23};
    const uint8_t waveform = partial ? fast[bucket] : full[bucket];
    if (partial && d->waveform_valid && d->waveform_partial && d->waveform == waveform)
        return ESP_OK;
    TRY(command(d, 0xe0, (uint8_t[]){0x12}, 1));
    TRY(command(d, 0xe6, &waveform, 1));
    TRY(command(d, 0xa5, NULL, 0));
    TRY(wait_ready(d));
    delay_ms(10);
    d->waveform = waveform;
    d->waveform_partial = partial;
    d->waveform_valid = true;
    return ESP_OK;
}

static inline __attribute__((always_inline)) uint16_t expand_wire_byte(uint8_t value)
{
    /* Native row bit 0 is source X0; the first wire pixel is the high pair. */
    static const uint8_t nibble[] = {0,0x40,0x10,0x50,4,0x44,0x14,0x54,1,0x41,0x11,0x51,5,0x45,0x15,0x55};
    return (uint16_t)((uint16_t)nibble[value & 15U] << 8U) | nibble[value >> 4U];
}

typedef struct {
    uint16_t x_start, x_end, y_start, y_end;
} ssd2677_window_t;

static esp_err_t select_window(epd_ssd2677_t *d, const ssd2677_window_t *w)
{
    /* PTLW (83h), SSD2677 rev 1.0 p24. Gate scanning still covers the
     * panel; RAM data and driven sources are confined to this rectangle. */
    const uint8_t address[] = {w->x_start>>8,w->x_start,w->x_end>>8,w->x_end,
        w->y_start>>8,w->y_start,w->y_end>>8,w->y_end,1};
    return command(d,0x83,address,sizeof(address));
}

static esp_err_t write_encoded_rectangle(epd_ssd2677_t *d, const uint8_t *image,
                                         const uint8_t *previous, const ssd2677_window_t *w)
{
    TRY(command(d, 0x10, NULL, 0));
    TRY(gpio_set_level(d->dc_pin, 1));
    if (d->line_buffer_size < SSD2677_TRANSFER_BYTES) return ESP_ERR_INVALID_SIZE;
    const unsigned row_bytes = (w->x_end-w->x_start+1U)/4U;
    for (unsigned y = w->y_start; y <= w->y_end; y += 8U) {
        /* Reverse the V2 gate rows without mirroring source X. Transpose each
         * 8x8 u8g2 block once and batch eight physical rows per SPI transfer. */
        const size_t tile = (SSD2677_HEIGHT - 1U - y) / 8U * SSD2677_WIDTH;
        for (unsigned x = w->x_start; x <= w->x_end; x += 8U) {
            uint32_t low, high, old_low, old_high;
            memcpy(&low, image + tile + x, 4);
            memcpy(&high, image + tile + x + 4U, 4);
            transpose_block(&low, &high);
            old_low = low; old_high = high;
            if (previous) {
                memcpy(&old_low, previous + tile + x, 4);
                memcpy(&old_high, previous + tile + x + 4U, 4);
                transpose_block(&old_low, &old_high);
            }
            for (unsigned row = 0; row < 8U; row++) {
                const unsigned bit = 7U - row, shift = (bit & 3U) * 8U;
                const uint8_t current = (uint8_t)((bit < 4U ? low : high) >> shift);
                const uint8_t old = (uint8_t)((bit < 4U ? old_low : old_high) >> shift);
                const uint16_t packed = (uint16_t)((expand_wire_byte(old) << 1U) | expand_wire_byte(current));
                const size_t offset = row * row_bytes + (x-w->x_start) / 4U;
                d->line_buffer[offset] = (uint8_t)(packed >> 8U);
                d->line_buffer[offset + 1U] = (uint8_t)packed;
            }
        }
        spi_transaction_t tx = {.length = row_bytes * 8U * 8U, .tx_buffer = d->line_buffer};
        TRY(transmit(d, &tx, write_clock(d)));
    }
    d->last_transfer_bytes = row_bytes*(w->y_end-w->y_start+1U);
    return ESP_OK;
}

static esp_err_t write_encoded_image(epd_ssd2677_t *d, const uint8_t *image, const uint8_t *previous)
{
    const ssd2677_window_t full = {0,SSD2677_WIDTH-1U,0,SSD2677_HEIGHT-1U};
    return write_encoded_rectangle(d,image,previous,&full);
}

static esp_err_t write_image(epd_ssd2677_t *d, const uint8_t *image, bool partial)
{
    return write_encoded_image(d,image,partial ? d->shadow : NULL);
}

static esp_err_t refresh_image(epd_ssd2677_t *d, const uint8_t *image, bool force_full)
{
    const int64_t started = esp_timer_get_time();
    if (force_full && d->refresh_mode == EPD_SSD2677_REFRESH_PULSE) d->pulse_needs_cleanup = true;
    if (d->fastwave_loaded) {
        TRY(powerdown(d));
        d->controller_ready = false;
    }
    if (!d->controller_ready) TRY(controller_init(d));
    if (!force_full && d->shadow_valid && d->shadow &&
        !memcmp(image, d->shadow, SSD2677_BUFFER_BYTES)) return ESP_OK;
    const bool partial = !force_full && d->shadow_valid && d->shadow &&
        d->refresh_mode != EPD_SSD2677_REFRESH_FULL &&
        (d->refresh_mode == EPD_SSD2677_REFRESH_PARTIAL || d->partial_refresh_count < 19U);
    TRY(latch_waveform(d, partial));
    const int64_t transfer_started = esp_timer_get_time();
    /* This panel profile currently transfers partial updates as full-frame diffs. */
    TRY(write_image(d, image, partial));
    d->last_transfer_us = (uint32_t)(esp_timer_get_time() - transfer_started);
    if (!d->analog_on) {
        /* Mark possibly-on before transmission so failure cleanup powers off. */
        d->analog_on = true;
        TRY(command(d, 0x04, NULL, 0));
        TRY(wait_ready(d));
    }
    /* SSD2677 rev 1.0 p20: 0x05 selects fixed 85 Hz; 0x08 restores the
     * factory dynamic rate. Full cleanup always retains factory timing. */
    const uint8_t pll = partial && d->refresh_mode == EPD_SSD2677_REFRESH_FAST ? 0x05 : 0x08;
    TRY(command(d, 0x30, &pll, 1));
    const int64_t drive_started = esp_timer_get_time();
    TRY(command(d, 0x12, (uint8_t[]){0}, 1));
    TRY(wait_ready(d));
    d->last_drive_us = (uint32_t)(esp_timer_get_time() - drive_started);
    if (!partial) TRY(powerdown(d));
    if (d->shadow) memcpy(d->shadow, image, SSD2677_BUFFER_BYTES);
    d->shadow_valid = d->shadow != NULL;
    d->partial_refresh_count = partial ? d->partial_refresh_count + 1U : 0;
    d->last_frame_us = (uint32_t)(esp_timer_get_time() - started);
    return ESP_OK;
}

static void invalidate(epd_ssd2677_t *d, esp_err_t err)
{
    d->last_error = err;
    if (err != ESP_OK) {
        d->controller_ready = d->shadow_valid = false;
        d->temperature_valid = d->waveform_valid = false;
    }
}

static void scan_accepting(epd_ssd2677_t *d)
{
    const bool enabled = d->refresh_mode == EPD_SSD2677_REFRESH_PULSE && d->scan_async &&
        !d->suspended && !d->power_state_present && !d->relax.storage &&
        d->powered && d->controller_ready;
    epd_scan_worker_enable(&d->scan_worker,enabled);
    __atomic_store_n(&d->scan_submit_ready,enabled,__ATOMIC_RELEASE);
}

static void scan_disable(epd_ssd2677_t *d)
{
    __atomic_store_n(&d->scan_submit_ready,false,__ATOMIC_RELEASE);
    epd_scan_worker_enable(&d->scan_worker,false);
}

static void free_budget(epd_ssd2677_t *d)
{
    d->pulse_window_valid = false;
    solar_os_memory_free(d->pixel_budget.active);
    solar_os_memory_free(d->pixel_budget.drive_history);
    solar_os_memory_free(d->pixel_budget.pixels);
    solar_os_memory_free(d->pulse_planes);
    memset(&d->pixel_budget,0,sizeof(d->pixel_budget));
    d->pulse_planes = NULL;
}

static esp_err_t allocate_budget(epd_ssd2677_t *d)
{
    if (d->pixel_budget.pixels) return ESP_OK;
    if (!d->shadow) return ESP_ERR_NO_MEM;
    const size_t count = SSD2677_WIDTH*SSD2677_HEIGHT;
    uint8_t *pixels = solar_os_memory_alloc(count,SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"ssd2677.budget");
    uint32_t *active = solar_os_memory_alloc(count*sizeof(*active),SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"ssd2677.active");
    uint16_t *history = solar_os_memory_alloc(count*sizeof(*history),SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"ssd2677.history");
    d->pulse_planes = solar_os_memory_alloc(SSD2677_BUFFER_BYTES*2U,SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"ssd2677.pulse");
    if (!pixels || !active || !history || !d->pulse_planes) {
        solar_os_memory_free(pixels); solar_os_memory_free(active); solar_os_memory_free(history);
        solar_os_memory_free(d->pulse_planes); d->pulse_planes = NULL;
        return ESP_ERR_NO_MEM;
    }
    TRY(epd_pixel_budget_init(&d->pixel_budget,pixels,count));
    TRY(epd_pixel_budget_set_active_storage(&d->pixel_budget,active,count));
    TRY(epd_pixel_budget_set_drive_history(&d->pixel_budget,history,count));
    d->pulse_needs_cleanup = true;
    return ESP_OK;
}

static esp_err_t load_fastwave(epd_ssd2677_t *d)
{
    if (d->fastwave_loaded) return ESP_OK;
    uint8_t lut[SOLAR_OS_SSD2677_LUT_BYTES];
    if (!solar_os_ssd2677_waveform_build(lut,d->pulse_tuning.pulse_frames,d->invert))
        return ESP_ERR_INVALID_ARG;
    /* Clear the factory waveform latch before RAM upload. Keep PSR LUT_EN=0:
     * analog rail settings continue to come from this panel's own OTP. */
    const bool shadow_valid = d->shadow_valid;
    TRY(powerdown(d));
    TRY(controller_init(d));
    d->shadow_valid = shadow_valid;
    TRY(command(d,0x20,lut,sizeof(lut)));
    TRY(command(d,0x30,&d->pulse_tuning.waveform_fr,1));
    d->fastwave_loaded = true;
    return ESP_OK;
}

static bool pixel_white(const uint8_t *source, size_t p)
{
    /* Budget order follows u8g2 tile bytes. Adjacent budget/history entries
     * then share cache lines and the hot mask loop needs no pixel division. */
    return (source[p >> 3U] & (1U << (p & 7U))) != 0;
}

static bool cleanup_due(const epd_ssd2677_t *d)
{
    const epd_pulse_tuning_t *t = &d->pulse_tuning;
    return d->pulse_updates >= t->cleanup_every * (t->cleanup_adaptive ? 4U : 1U) ||
        (t->cleanup_adaptive && d->pixel_budget.peak_drive_cycles >= t->cleanup_every * t->drive_cycles);
}

static esp_err_t pulse_prepare(epd_ssd2677_t *d, const uint8_t *source,
                                bool fresh, bool *baseline)
{
    *baseline = false;
    TRY(allocate_budget(d));
    if (!d->controller_ready || !d->shadow_valid || d->pulse_needs_cleanup || cleanup_due(d)) {
        TRY(refresh_image(d, source, true));
        epd_pixel_budget_reset(&d->pixel_budget);
        for (size_t p = 0; p < d->pixel_budget.count; ++p)
            epd_pixel_budget_seed(&d->pixel_budget, p, pixel_white(source, p));
        d->pulse_needs_cleanup = false;
        d->pulse_mask_dirty = true;
        d->pulse_window_valid = false;
        d->pulse_updates = 0;
        d->pulse_last_frame_start = 0;
        *baseline = true;
        return ESP_OK;
    }
    if (fresh && memcmp(source, d->shadow, SSD2677_BUFFER_BYTES) != 0) {
        if (!d->scan_async && d->pulse_last_frame_start) {
            const int64_t due = d->pulse_last_frame_start + (int64_t)d->scan_period_ms * 1000;
            while (esp_timer_get_time() < due) delay_ms(1);
        }
        d->pulse_last_frame_start = esp_timer_get_time();
        for (size_t i = 0; i < SSD2677_BUFFER_BYTES; ++i) {
            const uint8_t changed = source[i] ^ d->shadow[i];
            if (!changed) continue;
            for (unsigned bit = 0; bit < 8U; bit++) {
                if (changed & (1U << bit)) {
                    const size_t p = i * 8U + bit;
                    epd_pixel_budget_target(&d->pixel_budget, p,
                        (source[i] & (1U << bit)) != 0, d->pulse_tuning.drive_cycles);
                }
            }
        }
        memcpy(d->shadow, source, SSD2677_BUFFER_BYTES);
        ++d->pulse_updates; /* changed targets, never physical settling scans */
        d->pulse_mask_dirty = true;
    }
    return ESP_OK;
}

static esp_err_t pulse_cycle(epd_ssd2677_t *d)
{
    TRY(load_fastwave(d));
    if (d->pulse_mask_dirty || d->pulse_mask_pending != d->pixel_budget.pending) {
        uint8_t *black = d->pulse_planes, *white = black + SSD2677_BUFFER_BYTES;
        ssd2677_window_t active = {SSD2677_WIDTH-1U,0,SSD2677_HEIGHT-1U,0};
        bool changed = false;
        memset(d->pulse_planes,0,SSD2677_BUFFER_BYTES*2U);
        for (size_t i = 0; i < d->pixel_budget.active_count; i++) {
            const size_t p = d->pixel_budget.active[i];
            const epd_pixel_drive_t drive = epd_pixel_budget_drive(&d->pixel_budget,p);
            if (drive == EPD_PIXEL_HOLD) continue;
            (drive == EPD_PIXEL_DRIVE_BLACK ? black : white)[p >> 3U] |= 1U << (p & 7U);
        }
        /* Find aligned bounds once per byte, instead of dividing coordinates
         * for every active pixel. Both masks are already in native tile order. */
        for (unsigned tile = 0; tile < SSD2677_HEIGHT/8U; tile++) {
            const unsigned physical_y = SSD2677_HEIGHT-(tile+1U)*8U;
            const size_t offset = tile*SSD2677_WIDTH;
            for (unsigned x = 0; x < SSD2677_WIDTH; x++) {
                if (!(black[offset+x] | white[offset+x])) continue;
                if (x < active.x_start) active.x_start = x;
                if (x > active.x_end) active.x_end = x;
                if (physical_y < active.y_start) active.y_start = physical_y;
                if (physical_y+7U > active.y_end) active.y_end = physical_y+7U;
                changed = true;
            }
        }
        if (!changed) return ESP_ERR_INVALID_STATE;
        active.x_start &= (uint16_t)~7U; active.x_end |= 7U;
        active.y_start &= (uint16_t)~7U; active.y_end |= 7U;
        ssd2677_window_t window = {0,SSD2677_WIDTH-1U,0,SSD2677_HEIGHT-1U};
        if (d->pulse_window_valid) {
            window = active;
            /* Also clear the preceding mask where pixels finished settling. */
            if (d->pulse_x_start < window.x_start) window.x_start = d->pulse_x_start;
            if (d->pulse_x_end > window.x_end) window.x_end = d->pulse_x_end;
            if (d->pulse_y_start < window.y_start) window.y_start = d->pulse_y_start;
            if (d->pulse_y_end > window.y_end) window.y_end = d->pulse_y_end;
        }
        const int64_t began = esp_timer_get_time();
        TRY(select_window(d,&window));
        TRY(write_encoded_rectangle(d,white,black,&window));
        d->last_transfer_us = (uint32_t)(esp_timer_get_time()-began);
        d->pulse_x_start = active.x_start; d->pulse_x_end = active.x_end;
        d->pulse_y_start = active.y_start; d->pulse_y_end = active.y_end;
        d->pulse_window_valid = true;
        d->pulse_mask_dirty = false;
        d->pulse_mask_pending = d->pixel_budget.pending;
    }
    if (!d->analog_on) {
        d->analog_on = true;
        TRY(command(d,0x04,NULL,0));
        TRY(wait_ready(d));
    }
    const int64_t began = esp_timer_get_time();
    TRY(command(d,0x12,(uint8_t[]){0},1));
    const int64_t deadline = esp_timer_get_time()+50000;
    while (gpio_get_level(d->busy_pin)) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        delay_ms(1);
    }
    TRY(wait_ready(d));
    d->last_drive_us = (uint32_t)(esp_timer_get_time()-began);
    if (!d->pulse_power_hold) TRY(powerdown(d));
    epd_pixel_budget_advance(&d->pixel_budget);
    return ESP_OK;
}

static void pulse_failed(epd_ssd2677_t *d)
{
    scan_disable(d);
    d->refresh_mode = EPD_SSD2677_REFRESH_AUTO;
    d->controller_ready = d->shadow_valid = false;
    d->pulse_needs_cleanup = true;
}

static esp_err_t scan_process(void *context, const uint8_t *source,
                              bool fresh, bool *pending, bool *scanned)
{
    epd_ssd2677_t *d = context;
    *pending = *scanned = false;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (__atomic_load_n(&d->scan_submit_ready, __ATOMIC_ACQUIRE) && !d->suspended &&
        d->refresh_mode == EPD_SSD2677_REFRESH_PULSE && d->scan_async) {
        bool baseline;
        err = pulse_prepare(d, source, fresh, &baseline);
        if (err == ESP_OK && !baseline && d->pixel_budget.pending) {
            err = pulse_cycle(d);
            *scanned = err == ESP_OK;
        }
        if (err == ESP_OK && !d->pixel_budget.pending) err = powerdown(d);
        if (err != ESP_OK) {
            pulse_failed(d);
            /* Recover the newest accepted image even if the app stops painting. */
            xSemaphoreTake(d->scan_worker.lock, portMAX_DELAY);
            memcpy(d->scan_worker.snapshot, d->scan_worker.latest, SSD2677_BUFFER_BYTES);
            xSemaphoreGive(d->scan_worker.lock);
            if (refresh_image(d, d->scan_worker.snapshot, true) != ESP_OK)
                d->controller_ready = d->shadow_valid = false;
        } else *pending = d->pixel_budget.pending != 0 || cleanup_due(d);
        d->last_error = err;
    }
    if (d->lock) xSemaphoreGive(d->lock);
    return err;
}

static esp_err_t refresh_frame(epd_ssd2677_t *d)
{
    if (d->refresh_mode != EPD_SSD2677_REFRESH_PULSE)
        return refresh_image(d, d->buffer, false);
    bool baseline;
    TRY(pulse_prepare(d, d->buffer, true, &baseline));
    if (!baseline) while (d->pixel_budget.pending) TRY(pulse_cycle(d));
    TRY(powerdown(d));
    if (cleanup_due(d)) TRY(pulse_prepare(d, d->buffer, false, &baseline));
    return ESP_OK;
}


static esp_err_t refresh(epd_ssd2677_t *d)
{
    if (!d || !d->spi || !d->buffer || !d->line_buffer) return ESP_ERR_INVALID_STATE;
    if (__atomic_load_n(&d->scan_submit_ready, __ATOMIC_ACQUIRE) &&
        epd_scan_worker_submit(&d->scan_worker,d->buffer) == ESP_OK) return ESP_OK;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err = d->suspended || d->power_state_present ? ESP_ERR_INVALID_STATE :
        d->relax.storage ? ESP_OK : refresh_frame(d);
    if (err != ESP_OK && d->refresh_mode == EPD_SSD2677_REFRESH_PULSE) pulse_failed(d);
    invalidate(d, err);
    if (err == ESP_OK) scan_accepting(d);
    xSemaphoreGive(d->lock);
    return err;
}

static esp_err_t sleep_panel(epd_ssd2677_t *d)
{
    scan_disable(d);
    esp_err_t err = ESP_OK;
    if (d->powered) {
        err = powerdown(d);
        if (err == ESP_OK && d->controller_ready)
            err = command(d, 0x07, (uint8_t[]){0xa5}, 1);
        (void)gpio_set_level(d->reset_pin, 0);
        (void)gpio_set_level(d->dc_pin, 0);
        const esp_err_t power = set_power(d, false);
        if (err == ESP_OK) err = power;
    }
    d->suspended = true;
    d->controller_ready = d->shadow_valid = d->analog_on = false;
    return err;
}

static const u8x8_display_info_t display_info = {
    .chip_enable_level = 0, .chip_disable_level = 1,
    .reset_pulse_width_ms = 20, .post_reset_wait_ms = 50,
    .sck_clock_hz = 20000000, .spi_mode = 0,
    .tile_width = SSD2677_WIDTH / 8U, .tile_height = SSD2677_HEIGHT / 8U,
    .pixel_width = SSD2677_WIDTH, .pixel_height = SSD2677_HEIGHT,
};

static uint8_t display_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg, void *ptr)
{
    (void)ptr;
    if (msg == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &display_info);
        return 1;
    }
    epd_ssd2677_t *d = (epd_ssd2677_t *)((uint8_t *)u8x8 -
        offsetof(epd_ssd2677_t, u8g2) - offsetof(u8g2_t, u8x8));
    if (msg == U8X8_MSG_DISPLAY_DRAW_TILE) return 1;
    if (msg == U8X8_MSG_DISPLAY_REFRESH) return refresh(d) == ESP_OK;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err;
    if (msg == U8X8_MSG_DISPLAY_INIT) err = controller_init(d);
    else if (msg == U8X8_MSG_DISPLAY_SET_POWER_SAVE) {
        if (arg) err = sleep_panel(d);
        else {
            d->suspended = d->power_state_present = false;
            err = d->controller_ready ? ESP_OK : controller_init(d);
        }
    } else { xSemaphoreGive(d->lock); return 0; }
    invalidate(d, err);
    if (err == ESP_OK) scan_accepting(d);
    xSemaphoreGive(d->lock);
    return err == ESP_OK;
}

esp_err_t epd_ssd2677_init(epd_ssd2677_t *d, const epd_ssd2677_config_t *c)
{
    if (!d || !c || !c->rotation || c->spi_clock_hz < 100000 || c->spi_clock_hz > 20000000 ||
        c->busy_level != 0 || c->panel > EPD_SSD2677_PANEL_STICKY ||
        (c->power_active_level != 0 && c->power_active_level != 1)) return ESP_ERR_INVALID_ARG;
    const int pins[] = {c->cs_pin,c->dc_pin,c->reset_pin,c->busy_pin,c->power_pin};
    for (unsigned i = 0; i < 5; i++) {
        if (i == 4 && pins[i] == -1) continue;
        if (pins[i] < 0 || pins[i] >= GPIO_NUM_MAX ||
            (i == 3 ? !GPIO_IS_VALID_GPIO(pins[i]) : !GPIO_IS_VALID_OUTPUT_GPIO(pins[i])))
            return ESP_ERR_INVALID_ARG;
        for (unsigned j = 0; j < i; j++) if (pins[j] == pins[i]) return ESP_ERR_INVALID_ARG;
    }
    if ((!c->spi_bus || !c->spi_bus[0]) &&
        (!GPIO_IS_VALID_OUTPUT_GPIO(c->sclk_pin) || !GPIO_IS_VALID_OUTPUT_GPIO(c->mosi_pin) ||
         (c->spi_host != SPI2_HOST && c->spi_host != SPI3_HOST))) return ESP_ERR_INVALID_ARG;
    memset(d, 0, sizeof(*d));
    d->dc_pin = c->dc_pin; d->reset_pin = c->reset_pin; d->busy_pin = c->busy_pin;
    d->power_pin = c->power_pin; d->power_active_level = c->power_active_level;
    d->set_power = c->set_power; d->power_context = c->power_context; d->spi_host = c->spi_host;
    d->spi_clock_hz = c->spi_clock_hz;
    d->lock = xSemaphoreCreateMutexStatic(&d->lock_storage);
    d->mode_lock = xSemaphoreCreateMutexStatic(&d->mode_lock_storage);
    if (!d->lock || !d->mode_lock) return ESP_ERR_NO_MEM;
    d->pulse_tuning = (epd_pulse_tuning_t){.drive_cycles=8,.pulse_frames=4,
        .waveform_fr=5,.cleanup_every=64,.cleanup_adaptive=true};
    d->scan_async = d->pulse_power_hold = true;
    d->scan_period_ms = 100;
    d->pulse_spi_clock_hz = c->spi_clock_hz;
    uint64_t outputs = (1ULL << c->dc_pin) | (1ULL << c->reset_pin);
    if (c->power_pin >= 0) outputs |= 1ULL << c->power_pin;
    gpio_config_t gpio = {.pin_bit_mask = outputs, .mode = GPIO_MODE_OUTPUT};
    esp_err_t err = gpio_config(&gpio);
    gpio = (gpio_config_t){.pin_bit_mask = 1ULL << c->busy_pin, .mode = GPIO_MODE_INPUT};
    if (err == ESP_OK) err = gpio_config(&gpio);
    if (err == ESP_OK) err = set_power(d, false);
    const spi_device_interface_config_t device = {
        .clock_speed_hz = c->spi_clock_hz, .mode = 0, .spics_io_num = c->cs_pin, .queue_size = 1,
        .flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
    };
    if (err == ESP_OK && c->spi_bus && c->spi_bus[0])
        err = solar_os_bus_spi_add_device(c->spi_bus, &device, &d->spi);
    else if (err == ESP_OK) {
        const spi_bus_config_t bus = {.mosi_io_num = c->mosi_pin, .miso_io_num = -1,
            .sclk_io_num = c->sclk_pin, .quadwp_io_num = -1, .quadhd_io_num = -1,
            .max_transfer_sz = SSD2677_TRANSFER_BYTES};
        err = spi_bus_initialize(c->spi_host, &bus, SPI_DMA_CH_AUTO);
        if (err == ESP_OK) { d->bus_initialized = true; err = spi_bus_add_device(c->spi_host, &device, &d->spi); }
    }
    if (err == ESP_OK) {
        d->buffer_size = d->shadow_size = SSD2677_BUFFER_BYTES;
        d->line_buffer_size = SSD2677_TRANSFER_BYTES;
        d->buffer = solar_os_memory_calloc(1, d->buffer_size,
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "ssd2677.frame");
        d->shadow = solar_os_memory_alloc(d->shadow_size,
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "ssd2677.shadow");
        d->line_buffer = solar_os_memory_alloc(d->line_buffer_size,
            SOLAR_OS_MEMORY_DMA, "ssd2677.spi");
        if (!d->buffer || !d->line_buffer) err = ESP_ERR_NO_MEM;
        if (!d->shadow) d->shadow_size = 0;
    }
    if (err == ESP_OK) {
        u8g2_SetupDisplay(&d->u8g2, display_cb, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
        u8g2_SetupBuffer(&d->u8g2, d->buffer, SSD2677_HEIGHT / 8U,
            u8g2_ll_hvline_vertical_top_lsb, c->rotation);
        u8g2_InitDisplay(&d->u8g2);
        err = d->last_error;
        if (err == ESP_OK && c->panel == EPD_SSD2677_PANEL_STICKY) {
            const esp_err_t pulse = epd_ssd2677_set_controller_mode(d,"refresh=pulse");
            if (pulse != ESP_OK) ESP_LOGW("ssd2677","startup pulse unavailable: %s",esp_err_to_name(pulse));
        }
    }
    if (err != ESP_OK) epd_ssd2677_deinit(d);
    return err;
}

void epd_ssd2677_deinit(epd_ssd2677_t *d)
{
    /* A rejected configuration leaves the service's zeroed instance untouched;
     * cleanup must not reset GPIO0 from that uninitialized instance. */
    if (!d || !d->lock) return;
    scan_disable(d);
    epd_scan_worker_deinit(&d->scan_worker);
    free_budget(d);
    if (d->spi) {
        if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
        (void)sleep_panel(d);
        if (d->lock) xSemaphoreGive(d->lock);
        (void)spi_bus_remove_device(d->spi);
        d->spi = NULL;
    }
    if (d->bus_initialized) { (void)spi_bus_free(d->spi_host); d->bus_initialized = false; }
    epd_relax_dispose(&d->relax);
    solar_os_memory_free(d->buffer); solar_os_memory_free(d->shadow); solar_os_memory_free(d->line_buffer);
    d->buffer = d->shadow = d->line_buffer = NULL;
    d->buffer_size = d->shadow_size = d->line_buffer_size = 0;
    (void)gpio_reset_pin(d->dc_pin); (void)gpio_reset_pin(d->reset_pin);
    (void)gpio_reset_pin(d->busy_pin);
    if (d->power_pin >= 0) (void)gpio_reset_pin(d->power_pin);
    d->lock = NULL;
}

esp_err_t epd_ssd2677_resume(epd_ssd2677_t *d)
{
    if (!d || !d->spi || !d->buffer) return ESP_ERR_INVALID_STATE;
    scan_disable(d);
    xSemaphoreTake(d->lock, portMAX_DELAY);
    d->suspended = d->power_state_present = false;
    esp_err_t err = powerdown(d);
    if (err == ESP_OK) err = controller_init(d);
    invalidate(d, err);
    if (err == ESP_OK) scan_accepting(d);
    xSemaphoreGive(d->lock);
    return err;
}

u8g2_t *epd_ssd2677_get_u8g2(epd_ssd2677_t *d) { return d ? &d->u8g2 : NULL; }

static uint8_t reverse_byte(uint8_t value)
{
    value = ((value & 0x55U) << 1U) | ((value >> 1U) & 0x55U);
    value = ((value & 0x33U) << 2U) | ((value >> 2U) & 0x33U);
    return (value << 4U) | (value >> 4U);
}

static void transpose_block(uint32_t *low, uint32_t *high)
{
    uint32_t a = *low, b = *high;
    uint32_t swap = (a ^ (a >> 7U)) & 0x00aa00aaU;
    a ^= swap ^ (swap << 7U);
    swap = (b ^ (b >> 7U)) & 0x00aa00aaU;
    b ^= swap ^ (swap << 7U);
    swap = (a ^ (a >> 14U)) & 0x0000ccccU;
    a ^= swap ^ (swap << 14U);
    swap = (b ^ (b >> 14U)) & 0x0000ccccU;
    b ^= swap ^ (swap << 14U);
    swap = (a ^ (b << 4U)) & 0xf0f0f0f0U;
    *low = a ^ swap;
    *high = b ^ (swap >> 4U);
}

static bool blit_mono_packed(epd_ssd2677_t *d, const uint8_t *bitmap,
    unsigned x, unsigned y, unsigned width, unsigned height, unsigned stride,
    const u8g2_cb_t *rotation, bool inverted)
{
    if (rotation == U8G2_R0 || rotation == U8G2_R2) {
        if ((y | height) & 7U) return false;
        for (unsigned row = 0; row < height; row += 8U) {
            const unsigned native_y = rotation == U8G2_R0 ? y + row :
                SSD2677_HEIGHT - y - row - 8U;
            uint8_t *out = d->buffer + (native_y / 8U) * SSD2677_WIDTH;
            for (unsigned column = 0; column < width; column += 8U) {
                const uint8_t *source = bitmap + row * stride + column / 8U;
                uint32_t low = (uint32_t)source[0] | ((uint32_t)source[stride] << 8U) |
                    ((uint32_t)source[2U * stride] << 16U) | ((uint32_t)source[3U * stride] << 24U);
                uint32_t high = (uint32_t)source[4U * stride] | ((uint32_t)source[5U * stride] << 8U) |
                    ((uint32_t)source[6U * stride] << 16U) | ((uint32_t)source[7U * stride] << 24U);
                transpose_block(&low, &high);
                const unsigned count = width - column < 8U ? width - column : 8U;
                for (unsigned i = 0; i < count; i++) {
                    uint8_t value = (uint8_t)((i < 4U ? low : high) >> ((i & 3U) * 8U));
                    const unsigned nx = rotation == U8G2_R0 ? x + column + i :
                        SSD2677_WIDTH - 1U - x - column - i;
                    if (rotation == U8G2_R2) value = reverse_byte(value);
                    out[nx] = inverted ? value : (uint8_t)~value;
                }
            }
        }
    } else {
        if ((x | width) & 7U) return false;
        for (unsigned row = 0; row < height; row++) {
            const uint8_t *source = bitmap + row * stride;
            const unsigned nx = rotation == U8G2_R1 ? SSD2677_WIDTH - 1U - y - row : y + row;
            for (unsigned column = 0; column < width; column += 8U) {
                const unsigned ny = rotation == U8G2_R1 ? x + column : SSD2677_HEIGHT - x - column - 8U;
                uint8_t value = source[column / 8U];
                if (rotation == U8G2_R3) value = reverse_byte(value);
                d->buffer[(ny / 8U) * SSD2677_WIDTH + nx] = inverted ? value : (uint8_t)~value;
            }
        }
    }
    return true;
}

esp_err_t epd_ssd2677_present_mono_xbm(epd_ssd2677_t *d,
    const uint8_t *bitmap, size_t bitmap_size, uint16_t x, uint16_t y,
    uint16_t width, uint16_t height, uint16_t stride, bool palette_inverted)
{
    if (!d || !d->buffer || !bitmap || !width || !height ||
        stride < ((size_t)width + 7U) / 8U || height > SIZE_MAX / stride ||
        bitmap_size < (size_t)height * stride ||
        (uint32_t)x + width > u8g2_GetDisplayWidth(&d->u8g2) ||
        (uint32_t)y + height > u8g2_GetDisplayHeight(&d->u8g2)) return ESP_ERR_INVALID_ARG;
    const u8g2_cb_t *rotation = d->u8g2.cb;
    if (rotation != U8G2_R0 && rotation != U8G2_R1 &&
        rotation != U8G2_R2 && rotation != U8G2_R3) return ESP_ERR_NOT_SUPPORTED;
    if (!d->powered || d->suspended || d->power_state_present || d->relax.storage ||
        !d->spi || !d->line_buffer || d->buffer_size < SSD2677_BUFFER_BYTES) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(d->buffer, palette_inverted ? 0 : 0xff, SSD2677_BUFFER_BYTES);
    if (!blit_mono_packed(d, bitmap, x, y, width, height, stride, rotation, palette_inverted)) {
        for (unsigned row = 0; row < height; row++) {
            const uint8_t *source = bitmap + (size_t)row * stride;
            for (unsigned column = 0; column < width; column++) {
                const unsigned lx = x + column, ly = y + row;
                unsigned nx = lx, ny = ly;
                if (rotation == U8G2_R1) { nx = SSD2677_WIDTH - 1U - ly; ny = lx; }
                else if (rotation == U8G2_R2) {
                    nx = SSD2677_WIDTH - 1U - lx; ny = SSD2677_HEIGHT - 1U - ly;
                } else if (rotation == U8G2_R3) { nx = ly; ny = SSD2677_HEIGHT - 1U - lx; }
                uint8_t *destination = d->buffer + (ny / 8U) * SSD2677_WIDTH + nx;
                const uint8_t mask = (uint8_t)(1U << (ny & 7U));
                const bool black = (source[column / 8U] & (1U << (column & 7U))) != 0;
                if (black == palette_inverted) *destination |= mask;
                else *destination &= (uint8_t)~mask;
            }
        }
    }
    return refresh(d);
}

esp_err_t epd_ssd2677_present_power_state(epd_ssd2677_t *d)
{
    if (!d || !d->spi || !d->buffer || !d->line_buffer) return ESP_ERR_INVALID_STATE;
    scan_disable(d);
    xSemaphoreTake(d->lock, portMAX_DELAY);
    d->power_state_present = true;
    scan_disable(d);
    const esp_err_t err = d->suspended ? ESP_ERR_INVALID_STATE : refresh_image(d, d->buffer, true);
    invalidate(d, err);
    xSemaphoreGive(d->lock);
    return err;
}

static esp_err_t relax_native(void *context, solar_os_display_relax_phase_t phase, const uint8_t *image)
{
    epd_ssd2677_t *d = context;
    if (!d->powered || d->suspended || d->power_state_present) return ESP_ERR_INVALID_STATE;
    return phase == SOLAR_OS_DISPLAY_RELAX_BEGIN ? powerdown(d) : refresh_image(d, image, true);
}

esp_err_t epd_ssd2677_relax(epd_ssd2677_t *d, solar_os_display_relax_phase_t phase)
{
    if (!d || !d->buffer || !d->spi || (unsigned)phase > SOLAR_OS_DISPLAY_RELAX_END)
        return ESP_ERR_INVALID_ARG;
    if (phase == SOLAR_OS_DISPLAY_RELAX_BEGIN) scan_disable(d);
    xSemaphoreTake(d->lock, portMAX_DELAY);
    const esp_err_t err = phase == SOLAR_OS_DISPLAY_RELAX_BEGIN &&
        (!d->powered || d->suspended || d->power_state_present) ? ESP_ERR_INVALID_STATE :
        epd_relax_apply(&d->relax, d->buffer, d->buffer_size, phase, relax_native, d);
    invalidate(d, err);
    if (err == ESP_OK) scan_accepting(d);
    xSemaphoreGive(d->lock);
    return err;
}

const char *epd_ssd2677_controller_mode(const epd_ssd2677_t *display)
{
    if (!display) return NULL;
    epd_ssd2677_t *d = (epd_ssd2677_t *)display;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    const epd_scan_stats_t stats = epd_scan_worker_stats(&d->scan_worker);
    snprintf(d->mode_description, sizeof(d->mode_description),
        "refresh=%s waveform=%s drive-cycles=%u pulse-frames=%u waveform-fr=%u "
        "cleanup-every=%u cleanup-policy=%s cleanup-peak-cycles=%u cleanup-age=%u "
        "pulse-scan=%s pulse-period=%u pulse-power=%s pulse-spi-khz=%u invert=%s "
        "pending=%u scan-count=%u frame-count=%u scan-submitted=%u scan-coalesced=%u "
        "scan-last-us=%lld scan-interval-us=%lld temperature=%u partial-count=%u "
        "spi-khz=%u native-last-us=%u tx-us=%u tx-bytes=%u drive-us=%u",
        d->refresh_mode == EPD_SSD2677_REFRESH_FULL ? "full" :
        d->refresh_mode == EPD_SSD2677_REFRESH_PARTIAL ? "partial" :
        d->refresh_mode == EPD_SSD2677_REFRESH_FAST ? "fast" :
        d->refresh_mode == EPD_SSD2677_REFRESH_PULSE ? "pulse" : "auto",
        d->refresh_mode == EPD_SSD2677_REFRESH_PULSE ? "fastwave" : "otp",
        d->pulse_tuning.drive_cycles,d->pulse_tuning.pulse_frames,d->pulse_tuning.waveform_fr,
        d->pulse_tuning.cleanup_every,d->pulse_tuning.cleanup_adaptive ? "adaptive" : "fixed",
        (unsigned)d->pixel_budget.peak_drive_cycles,(unsigned)d->pulse_updates,
        d->scan_async ? "async" : "sync",d->scan_period_ms,d->pulse_power_hold ? "hold" : "cycle",
        (unsigned)(d->pulse_spi_clock_hz/1000),d->invert ? "true" : "false",
        (unsigned)d->pixel_budget.pending,(unsigned)stats.scans,(unsigned)stats.frames,
        (unsigned)stats.submitted,(unsigned)stats.coalesced,
        (long long)stats.last_scan_us,(long long)stats.last_interval_us,
        d->temperature, d->partial_refresh_count, (unsigned)(d->spi_clock_hz / 1000),
        (unsigned)d->last_frame_us, (unsigned)d->last_transfer_us,
        (unsigned)d->last_transfer_bytes, (unsigned)d->last_drive_us);
    xSemaphoreGive(d->lock);
    return d->mode_description;
}

const char *epd_ssd2677_controller_mode_values(const epd_ssd2677_t *d)
{
    (void)d;
    return "refresh=<auto,fast,partial,full,pulse> waveform=<otp,fastwave> "
        "drive-cycles=<1..32> pulse-frames=<1..32> waveform-fr=<1..5> "
        "cleanup-every=<1..128> cleanup-policy=<fixed,adaptive> "
        "pulse-scan=<sync,async> pulse-period=<67..1000> pulse-power=<hold,cycle> "
        "pulse-spi-khz=<100..20000> invert=<true,false>";
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

static esp_err_t set_controller_mode(epd_ssd2677_t *d, const char *mode)
{
    if (d->relax.storage || d->suspended || d->power_state_present) return ESP_ERR_INVALID_STATE;
    const epd_ssd2677_refresh_mode_t before = d->refresh_mode;
    bool restart = false;
    if (strcmp(mode, "refresh=auto") == 0 || strcmp(mode, "waveform=otp") == 0)
        d->refresh_mode = EPD_SSD2677_REFRESH_AUTO;
    else if (strcmp(mode, "refresh=fast") == 0) d->refresh_mode = EPD_SSD2677_REFRESH_FAST;
    else if (strcmp(mode, "refresh=partial") == 0)
        d->refresh_mode = EPD_SSD2677_REFRESH_PARTIAL;
    else if (strcmp(mode, "refresh=full") == 0) d->refresh_mode = EPD_SSD2677_REFRESH_FULL;
    else if (strcmp(mode, "refresh=pulse") == 0 || strcmp(mode, "waveform=fastwave") == 0) {
        const bool allocated = d->pixel_budget.pixels != NULL;
        TRY(allocate_budget(d));
        if (d->scan_async && !d->scan_worker.task) {
            const esp_err_t err = epd_scan_worker_init(&d->scan_worker, SSD2677_BUFFER_BYTES, scan_process, d);
            if (err != ESP_OK) {
                if (!allocated) free_budget(d);
                return err;
            }
            TRY(epd_scan_worker_set_period(&d->scan_worker, d->scan_period_ms));
        }
        d->refresh_mode = EPD_SSD2677_REFRESH_PULSE;
        restart = before != d->refresh_mode;
    } else if (strcmp(mode, "pulse-scan=async") == 0) {
        if (!d->scan_worker.task) {
            TRY(epd_scan_worker_init(&d->scan_worker, SSD2677_BUFFER_BYTES, scan_process, d));
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
    } else if (strncmp(mode, "pulse-period=", 13) == 0 || strncmp(mode, "pulse-spi-khz=", 14) == 0) {
        const bool spi = mode[6] == 's';
        unsigned value;
        if (!parse_tuning(mode + (spi ? 14 : 13), spi ? 20000 : 1000, &value) ||
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
        if (d->pulse_tuning.waveform_fr > 5) {
            d->pulse_tuning = previous;
            return ESP_ERR_INVALID_ARG;
        }
        restart = previous.pulse_frames != d->pulse_tuning.pulse_frames ||
            previous.waveform_fr != d->pulse_tuning.waveform_fr;
    }
    if (restart || (before == EPD_SSD2677_REFRESH_PULSE && before != d->refresh_mode))
        d->pulse_needs_cleanup = true;
    if (d->refresh_mode != EPD_SSD2677_REFRESH_PULSE && d->pixel_budget.pixels) {
        scan_disable(d);
        TRY(powerdown(d));
        d->controller_ready = d->shadow_valid = false;
        free_budget(d);
    }
    scan_accepting(d);
    if (d->refresh_mode != EPD_SSD2677_REFRESH_PULSE || !d->scan_async)
        TRY(powerdown(d));
    return ESP_OK;
}

esp_err_t epd_ssd2677_set_controller_mode(epd_ssd2677_t *d, const char *mode)
{
    if (!d || !mode) return ESP_ERR_INVALID_ARG;
    if (d->mode_lock) xSemaphoreTake(d->mode_lock, portMAX_DELAY);
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    const esp_err_t err = set_controller_mode(d, mode);
    const bool stop_worker = err == ESP_OK &&
        (d->refresh_mode != EPD_SSD2677_REFRESH_PULSE || !d->scan_async);
    if (stop_worker) scan_disable(d);
    if (d->lock) xSemaphoreGive(d->lock);
    /* Joining under the panel lock would deadlock an in-flight scan. */
    if (stop_worker) epd_scan_worker_deinit(&d->scan_worker);
    if (d->mode_lock) xSemaphoreGive(d->mode_lock);
    return err;
}
