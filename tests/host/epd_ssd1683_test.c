#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "solar_os_log.h"
#include "esp_log.h"
#undef ESP_LOGI
#define ESP_LOGI(tag, ...) do { (void)(tag); snprintf(last_log, sizeof(last_log), __VA_ARGS__); } while (0)
static char last_log[256];
static char error_log[256];
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...) {
    assert(strcmp(tag, "epd_ssd1683") == 0);
    va_list args;
    va_start(args, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, args);
    va_end(args);
    if (level == SOLAR_OS_LOG_LEVEL_ERROR)
        snprintf(error_log, sizeof(error_log), "%s", last_log);
    return ESP_OK;
}
#include "../../src/drivers/epd_ssd1683.c"

const u8g2_cb_t u8g2_cb_r0 = {0}, u8g2_cb_r1 = {0};
const u8g2_cb_t u8g2_cb_r2 = {0}, u8g2_cb_r3 = {0};

/* The shared worker's real concurrency is covered by epd_scan_worker_test.
 * Here deterministic ticks exercise the adapter's hardware/state contract.
 */
static bool worker_init_fail;
static bool worker_pending;
esp_err_t epd_scan_worker_init(epd_scan_worker_t *w, size_t size, epd_scan_process_fn fn, void *ctx) {
    if (worker_init_fail) return ESP_ERR_NO_MEM;
    memset(w, 0, sizeof(*w));
    w->lock = xSemaphoreCreateMutexStatic(&w->lock_storage);
    w->latest = calloc(1, size); w->snapshot = calloc(1, size);
    assert(w->latest && w->snapshot);
    w->task = (void *)1; w->frame_size = size; w->process = fn; w->context = ctx;
    w->period_ms = 100;
    return ESP_OK;
}
void epd_scan_worker_enable(epd_scan_worker_t *w, bool enabled) {
    if (!w->task) return;
    if (w->enabled != enabled) ++w->generation;
    w->enabled = enabled;
    if (!enabled) w->has_target = false;
}
esp_err_t epd_scan_worker_submit(epd_scan_worker_t *w, const uint8_t *frame) {
    if (!w->task || !w->enabled) return ESP_ERR_INVALID_STATE;
    if (w->has_target) ++w->stats.coalesced;
    memcpy(w->latest, frame, w->frame_size); w->has_target = true;
    ++w->stats.submitted; return ESP_OK;
}
esp_err_t epd_scan_worker_set_period(epd_scan_worker_t *w, unsigned ms) {
    if (!w->task) return ESP_ERR_INVALID_STATE;
    if (ms < 10 || ms > 1000) return ESP_ERR_INVALID_ARG;
    w->period_ms = ms; return ESP_OK;
}
epd_scan_stats_t epd_scan_worker_stats(epd_scan_worker_t *w) { return w->stats; }
void epd_scan_worker_deinit(epd_scan_worker_t *w) {
    free(w->latest); free(w->snapshot); w->latest = w->snapshot = NULL; w->task = NULL;
}
void u8x8_d_helper_display_setup_memory(u8x8_t *u8x8, const u8x8_display_info_t *info) {
    u8x8->display_info = info;
}
static esp_err_t worker_tick(epd_scan_worker_t *w) {
    bool fresh = w->has_target, pending = false, scanned = false;
    if (fresh) memcpy(w->snapshot, w->latest, w->frame_size);
    w->has_target = false;
    esp_err_t err = w->process(w->context, w->snapshot, fresh, &pending, &scanned);
    worker_pending = pending;
    if (scanned) ++w->stats.scans;
    return err;
}

typedef struct { uint8_t cmd; size_t size; uint8_t data[15000];
    int64_t at_us;
    unsigned x_start, x_end, y_start, y_end;
    unsigned entry_mode;
} capture_t;
static capture_t captures[256];
static size_t capture_count;
static int dc_level, resets;
static int64_t time_us, busy_until;
static bool no_busy, stuck_busy;
static bool green_controller;
static uint8_t fail_command;
static uint8_t frame[SSD1683_DUAL_BUFFER_BYTES], shadow[SSD1683_DUAL_BUFFER_BYTES];
static uint8_t line[SSD1683_DUAL_HEIGHT];
static unsigned ram_bounds[2][4];
static unsigned ram_entry[2], ram_height;
static uint32_t actual_spi_hz;
static uint8_t ram[2][2][50U * 300U];
static void (*activation_hook)(void);
static uint64_t sleep_retained;
static int retention_fail_pin = -1;

static uint8_t update_mode(void) {
    for (size_t i = capture_count; i > 0; --i)
        if (captures[i - 1].cmd == 0x22 && captures[i - 1].size == 1)
            return captures[i - 1].data[0];
    return 0;
}

void vTaskDelay(TickType_t ticks) {
    assert(ticks);
    time_us += ticks * SOLAR_OS_EPD_TEST_TICK_MS * 1000LL;
}
int64_t esp_timer_get_time(void) { return time_us; }
int gpio_get_level(gpio_num_t pin) {
    (void)pin;
    const bool active = stuck_busy || time_us < busy_until;
    return green_controller ? !active : active;
}
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t value) {
    if (pin == 46) dc_level = (int)value;
    if (pin == 47 && value == 0) { ++resets; busy_until = 0; }
    return ESP_OK;
}
esp_err_t gpio_set_direction(gpio_num_t pin, int mode) { (void)pin; (void)mode; return ESP_OK; }
esp_err_t gpio_set_pull_mode(gpio_num_t pin, int mode) { (void)pin; (void)mode; return ESP_OK; }
esp_err_t gpio_config(const gpio_config_t *config) { assert(config->pin_bit_mask); return ESP_OK; }
esp_err_t gpio_sleep_sel_dis(gpio_num_t pin) {
    assert(pin >= 0 && pin < GPIO_NUM_MAX);
    if (pin == retention_fail_pin) return ESP_FAIL;
    sleep_retained |= 1ULL << pin;
    return ESP_OK;
}
esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *t) {
    assert(device);
    if (t->override_freq_hz) actual_spi_hz = t->override_freq_hz;
    const uint8_t *data = t->flags & SPI_TRANS_USE_TXDATA ? t->tx_data : t->tx_buffer;
    const size_t size = t->length / 8U;
    if (!dc_level) {
        assert(size == 1 && capture_count < 256);
        const bool display_activation = !green_controller && *data == 0x20 && (update_mode() & 4);
        if (display_activation && activation_hook) {
            void (*hook)(void) = activation_hook; activation_hook = NULL; hook();
        }
        if (fail_command != 0 && *data == fail_command) { fail_command = 0; return ESP_FAIL; }
        captures[capture_count++] = (capture_t){.cmd = *data, .at_us = time_us};
        if ((*data & 0x7fU) == 0x24 || (*data & 0x7fU) == 0x26) {
            capture_t *c = &captures[capture_count - 1];
            unsigned *bounds = ram_bounds[(*data & 0x80U) != 0];
            c->x_start = bounds[0]; c->x_end = bounds[1];
            c->y_start = bounds[2]; c->y_end = bounds[3];
            c->entry_mode = ram_entry[(*data & 0x80U) != 0];
        }
        if (display_activation ||
            (green_controller && (*data == 0x04 || *data == 0x12 || *data == 0x02))) {
            if (!no_busy) busy_until = time_us + 20000;
        }
    } else {
        assert(capture_count);
        capture_t *c = &captures[capture_count - 1];
        assert(c->size + size <= sizeof(c->data));
        if (!green_controller && ((c->cmd & 0x7fU) == 0x24 || (c->cmd & 0x7fU) == 0x26)) {
            const unsigned chip = (c->cmd & 0x80U) != 0;
            const unsigned plane = (c->cmd & 0x7fU) == 0x26;
            const unsigned rows = (c->y_start > c->y_end ? c->y_start - c->y_end : c->y_end - c->y_start) + 1U;
            const unsigned columns = (c->x_start > c->x_end ? c->x_start - c->x_end : c->x_end - c->x_start) + 1U;
            for (size_t n = 0; n < size; ++n) {
                const unsigned pos = c->size + n;
                const unsigned column = c->entry_mode & 4 ? pos / rows : pos % columns;
                const unsigned row = c->entry_mode & 4 ? pos % rows : pos / columns;
                const unsigned x = c->entry_mode & 1 ? c->x_start + column : c->x_start - column;
                const unsigned y = c->entry_mode & 2 ? c->y_start + row : c->y_start - row;
                assert(x < 50 && y < ram_height);
                ram[chip][plane][x * ram_height + y] = data[n];
            }
        }
        memcpy(c->data + c->size, data, size);
        c->size += size;
        if (green_controller && c->cmd == 0x17 && !no_busy)
            busy_until = time_us + 20000;
        unsigned *bounds = ram_bounds[(c->cmd & 0x80U) != 0];
        if ((c->cmd & 0x7fU) == 0x11 && c->size == 1)
            ram_entry[(c->cmd & 0x80U) != 0] = c->data[0];
        if ((c->cmd & 0x7fU) == 0x44 && c->size == 2) {
            bounds[0] = c->data[0]; bounds[1] = c->data[1];
        }
        if ((c->cmd & 0x7fU) == 0x45 && c->size == 4) {
            bounds[2] = c->data[0] | (c->data[1] << 8);
            bounds[3] = c->data[2] | (c->data[3] << 8);
        }
    }
    return ESP_OK;
}

static epd_ssd1683_t display(void) {
    memset(frame, 0xff, sizeof(frame));
    memset(shadow, 0xff, sizeof(shadow));
    capture_count = 0; resets = 0; busy_until = 0;
    memset(ram, 0, sizeof(ram));
    memset(ram_entry, 0, sizeof(ram_entry)); ram_height = 272;
    actual_spi_hz = 10000000;
    no_busy = stuck_busy = false; fail_command = 0;
    green_controller = false;
    return (epd_ssd1683_t){.spi = (void *)1, .buffer = frame, .shadow = shadow,
        .line_buffer = line, .buffer_size = sizeof(frame), .shadow_size = sizeof(shadow),
        .line_buffer_size = sizeof(line), .cs_pin = 45, .dc_pin = 46, .reset_pin = 47,
        .busy_pin = 48, .busy_level = 1, .power_pin = 7, .powered = true,
        .panel_variant = EPD_SSD1683_PANEL_ELECROW_579_DUAL,
        .controller_ready = true, .shadow_valid = true,
        .scan_period_ms = 100,
        .spi_clock_hz = 10000000, .pulse_spi_clock_hz = 10000000, .spi_applied_hz = 10000000,
        .pulse_tuning = ssd1683_default_pulse_tuning(EPD_SSD1683_PANEL_ELECROW_579_DUAL)};
}
static capture_t *find(uint8_t cmd, unsigned occurrence) {
    for (size_t i = 0; i < capture_count; ++i)
        if (captures[i].cmd == cmd && occurrence-- == 0) return &captures[i];
    assert(!"missing command"); return NULL;
}
static unsigned count(uint8_t cmd) {
    unsigned n = 0;
    for (size_t i = 0; i < capture_count; ++i) n += captures[i].cmd == cmd;
    return n;
}
/* Clock-only enable and analog power-down also use 0x20. Count and time
 * optical updates separately from those non-display activations. */
static capture_t *display_update(unsigned occurrence, bool activation) {
    capture_t *mode = NULL;
    for (size_t i = 0; i < capture_count; ++i) {
        if (captures[i].cmd == 0x22) mode = &captures[i];
        if (captures[i].cmd == 0x20 && mode && (mode->data[0] & 4) && occurrence-- == 0)
            return activation ? &captures[i] : mode;
    }
    assert(!"missing optical update"); return NULL;
}
static unsigned display_update_count(void) {
    unsigned n = 0;
    uint8_t mode = 0;
    for (size_t i = 0; i < capture_count; ++i) {
        if (captures[i].cmd == 0x22) mode = captures[i].data[0];
        if (captures[i].cmd == 0x20 && (mode & 4)) ++n;
    }
    return n;
}
static void pixel(unsigned x, unsigned y, bool white) {
    const size_t p = (y / 8U) * SSD1683_DUAL_BUFFER_ROW_BYTES + x;
    const uint8_t bit = 1U << (y % 8U);
    frame[p] = white ? frame[p] | bit : frame[p] & (uint8_t)~bit;
}
static bool plane_pixel(uint8_t cmd, unsigned x, unsigned y) {
    /* Slave transport columns start at padded x=400; 8-pixel seam at 396. */
    unsigned padded_x = x < 396 ? x : x + 8;
    capture_t *c = find(cmd, 0);
    unsigned column = padded_x / 8U;
    unsigned local = cmd & 0x80U ? 99U - column : column;
    unsigned ram_y = 271U - y;
    if ((cmd & 0x80U ? local > c->x_start || local < c->x_end :
                       local < c->x_start || local > c->x_end) ||
        ram_y > c->y_start || ram_y < c->y_end)
        return false;
    unsigned col_index = cmd & 0x80U ? c->x_start - local : local - c->x_start;
    unsigned rows = c->y_start - c->y_end + 1U;
    unsigned row_index = c->y_start - ram_y;
    assert(col_index * rows + row_index < c->size);
    return (c->data[col_index * rows + row_index] & (0x80U >> (padded_x % 8U))) != 0;
}
static void free_budget(epd_ssd1683_t *d) {
    heap_caps_free(d->pixel_budget.active);
    heap_caps_free(d->pixel_budget.drive_history);
    heap_caps_free(d->pixel_budget.pixels);
    heap_caps_free(d->pulse_planes);
    d->pulse_planes = NULL;
    memset(&d->pixel_budget, 0, sizeof(d->pixel_budget));
}
static bool ram_pixel(unsigned plane, unsigned x, unsigned y) {
    unsigned padded = x < 396 ? x : x + 8;
    unsigned column = padded / 8U;
    unsigned chip = column >= 50;
    unsigned local = chip ? 99U - column : column;
    return (ram[chip][plane][local * 272U + 271U - y] & (0x80U >> (padded % 8U))) != 0;
}

static void test_native(void) {
    epd_ssd1683_t d = display();
    assert(ssd1683_refresh(&d) == ESP_OK && capture_count == 0);
    pixel(395, 0, false); pixel(396, 271, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(find(0x22, 0)->data[0] == 0xdc);
    assert(!plane_pixel(0x24, 395, 0) && !plane_pixel(0xa4, 396, 271));
    assert(count(0x32) == 0 && count(0xb2) == 0);
}
static void test_pulse_and_restore(void) {
    epd_ssd1683_t d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "drive-cycles=3") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK); /* Native full baseline, even unchanged. */
    assert(find(0x22, 0)->data[0] == 0xf7 && d.pixel_budget.pending == 0);
    capture_count = 0;
    pixel(0, 0, false); pixel(395, 100, false); pixel(396, 271, false); pixel(791, 10, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(count(0x32) == 1 && count(0xb2) == 1);
    /* Published panel power setup must precede its first custom activation,
     * for both controllers, rather than relying on previous OTP state.
     */
    assert(find(0x0c, 0)->size == 4 && find(0x8c, 0)->size == 4);
    assert(memcmp(find(0x0c, 0)->data, (uint8_t[]){0x8b,0x9c,0xa4,0x0f}, 4) == 0);
    assert(memcmp(find(0x8c, 0)->data, find(0x0c, 0)->data, 4) == 0);
    assert(find(0x03, 0)->data[0] == 0x17 && find(0x83, 0)->data[0] == 0x17);
    assert(find(0x04, 0)->size == 3 && find(0x84, 0)->size == 3);
    assert(memcmp(find(0x04, 0)->data, (uint8_t[]){0x41,0xa8,0x32}, 3) == 0);
    assert(memcmp(find(0x84, 0)->data, find(0x04, 0)->data, 3) == 0);
    capture_t *lut = find(0x32, 0), *slave_lut = find(0xb2, 0);
    assert(lut->size == 227 && memcmp(lut->data, slave_lut->data, 227) == 0);
    assert(lut->data[84] == 1 && lut->data[126] == 1);
    assert(lut->data[85] == 0 && lut->data[127] == 0); /* No reset kick. */
    assert(lut->data[86] == 0x48 && lut->data[128] == 0x88);
    assert(lut->data[87] == 1 && lut->data[129] == 1); /* VSS settle. */
    assert(lut->data[89] == 1 && lut->data[90] == 1);
    assert(lut->data[131] == 1 && lut->data[132] == 1);
    for (unsigned i = 0; i < 84; ++i) assert(lut->data[i] == 0);
    for (unsigned i = 168; i < 210; ++i) assert(lut->data[i] == 0);
    assert(lut->data[224] == 4);
    assert(solar_os_ssd1683_waveform_frames(lut->data) == 9);
    assert(find(0x22, 0)->data[0] == 0x80); /* Clock before loading both LUTs. */
    assert(find(0x20, 0) < lut && lut < display_update(0, true));
    assert(count(0x24) == 1 && count(0x26) == 1 && count(0xa4) == 1 && count(0xa6) == 1);
    assert(display_update_count() == 3); /* All drive cycles still complete. */
    assert(find(0x21, 0)->size == 2 && find(0x21, 0)->data[1] == 0x10);
    assert(find(0x37, 0)->size == 10 && find(0xb7, 0)->size == 10);
    assert(find(0x37, 0)->data[5] == 0 && find(0xb7, 0)->data[5] == 0);
    assert(find(0x2c, 0)->data[0] == 0x08 && find(0xac, 0)->data[0] == 0x08);
    assert(count(0x3f) == 0 && count(0xbf) == 0); /* EOPT is deliberately masked out. */
    assert(plane_pixel(0x26, 0, 0) && plane_pixel(0x26, 395, 100));
    assert(plane_pixel(0xa6, 396, 271) && plane_pixel(0xa6, 791, 10));
    assert(!plane_pixel(0x24, 0, 0) && !plane_pixel(0xa4, 396, 271));
    assert(!plane_pixel(0x26, 1, 0)); /* Unchanged white is HOLD, not driven. */
    for (unsigned y = 0; y < 272; ++y) {
        assert((find(0x26, 0)->data[49U * 272U + y] & 0x0fU) == 0);
        assert((find(0xa6, 0)->data[y] & 0xf0U) == 0);
    }
    assert(d.pixel_budget.pending == 0 && d.pulse_updates == 1);
    assert(strstr(last_log, "pixels=4 cycles=3") != NULL);
    capture_count = 0;
    assert(ssd1683_refresh(&d) == ESP_OK && capture_count == 0);
    pixel(395, 100, true); pixel(396, 271, true);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(plane_pixel(0x24, 395, 100) && plane_pixel(0xa4, 396, 271));
    assert(!plane_pixel(0x26, 395, 100) && !plane_pixel(0xa6, 396, 271));
    capture_count = 0;
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=auto") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(resets > 0 && !d.pulse_lut_loaded && d.pixel_budget.pixels == NULL);
    assert(count(0x32) == 0 && find(0x22, count(0x22) - 1)->data[0] == 0xf7);
}
static void test_default_drive_budget(void) {
    epd_ssd1683_t d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK);
    capture_count = 0;
    pixel(0, 0, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(display_update_count() == 6 && d.pixel_budget.pending == 0);
    assert(count(0x24) == 1 && count(0x26) == 1 && count(0xa4) == 1 && count(0xa6) == 1);
    capture_count = 0;
    pixel(0, 0, true);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(display_update_count() == 6 && d.pixel_budget.pending == 0);
    free_budget(&d);
}
static void test_settled_pixels_replace_mask(void) {
    epd_ssd1683_t d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK);
    pixel(0, 0, false); pixel(1, 0, false);
    /* Different remaining budgets exercise an active-mask change mid-frame.
     * Retargeting the same colors in refresh must preserve these budgets.
     */
    epd_pixel_budget_target(&d.pixel_budget, 0, false, 1);
    epd_pixel_budget_target(&d.pixel_budget, 1, false, 3);
    capture_count = 0;
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(display_update_count() == 3 && count(0x26) == 2 && count(0xa6) == 1);
    assert(plane_pixel(0x26, 0, 0) && plane_pixel(0x26, 1, 0));
    assert((find(0x26, 1)->data[0] & 0xc0U) == 0x40U);
    assert(d.pixel_budget.pending == 0);
    free_budget(&d);
}
static void test_long_phase_voltage_bits(void) {
    epd_ssd1683_t d = display();
    for (unsigned fr = 6; fr <= 8; ++fr) {
        char setting[32]; snprintf(setting, sizeof(setting), "waveform-fr=%u", fr);
        assert(epd_ssd1683_set_controller_mode(&d, setting) == ESP_ERR_INVALID_ARG);
        assert(d.pulse_tuning.waveform_fr == 4 && !d.pulse_needs_cleanup);
    }
    assert(strstr(epd_ssd1683_controller_mode(&d), "waveform=fastwave"));
    assert(strstr(epd_ssd1683_controller_mode_values(&d), "waveform-fr=<1..5,9..12>"));
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-frames=32") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "drive-cycles=1") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK);
    capture_count = 0;
    pixel(0, 0, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    /* Six duration bits must never overwrite the two voltage-selection bits. */
    assert(find(0x32, 0)->data[86] == 0x60);
    assert(find(0x32, 0)->data[128] == 0xa0);
    assert(memcmp(find(0x32, 0)->data, find(0xb2, 0)->data, 227) == 0);
    free_budget(&d);
}
static void test_moving_window_clears_old_mask(void) {
    epd_ssd1683_t d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "drive-cycles=1") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK);
    capture_count = 0;
    pixel(5, 10, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    /* First custom activation replaces native RAM globally, making settled
     * white pixels neutral rather than accidentally selecting white drive.
     */
    assert(count(0x24) == 1 && count(0xa4) == 1);
    assert(find(0x24, 0)->size == 13600 && find(0xa4, 0)->size == 13600);
    assert(!ram_pixel(0, 700, 100) && !ram_pixel(1, 700, 100));
    assert(ram_pixel(1, 5, 10));
    capture_count = 0;
    pixel(5, 10, true); pixel(405, 12, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(ram_pixel(0, 5, 10) && !ram_pixel(1, 5, 10));
    assert(!ram_pixel(0, 405, 12) && ram_pixel(1, 405, 12));
    capture_count = 0;
    pixel(405, 12, true);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(!ram_pixel(0, 5, 10) && !ram_pixel(1, 5, 10));
    assert(ram_pixel(0, 405, 12) && !ram_pixel(1, 405, 12));
    capture_count = 0;
    pixel(420, 13, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    /* A later right-only move clears the preceding drive and shrinks to
     * three padded columns by two rows, without rewriting the left chip.
     */
    assert(count(0x24) == 0 && count(0x26) == 0);
    assert(find(0xa4, 0)->size == 6 && find(0xa6, 0)->size == 6);
    assert(!ram_pixel(0, 405, 12) && !ram_pixel(1, 405, 12));
    assert(!ram_pixel(0, 420, 13) && ram_pixel(1, 420, 13));
    capture_count = 0;
    assert(ssd1683_refresh(&d) == ESP_OK && capture_count == 0);
    free_budget(&d);
}
static epd_ssd1683_t *hook_display;
static void submit_during_activation(void) {
    pixel(3, 0, false);
    const size_t before = capture_count;
    assert(ssd1683_u8x8_display_cb(&hook_display->u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(capture_count == before); /* Submission must not perform another scan. */
}
static void test_async_retarget_and_recovery(void) {
    epd_ssd1683_t d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "drive-cycles=3") == ESP_OK);
    worker_init_fail = true;
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-scan=async") == ESP_ERR_NO_MEM);
    assert(!d.scan_async); worker_init_fail = false;
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-scan=async") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(capture_count == 0 && d.pixel_budget.pixels == NULL);
    assert(worker_tick(&d.scan_worker) == ESP_OK); /* Native baseline. */
    capture_count = 0;
    pixel(0, 0, false);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(capture_count == 0);
    pixel(0, 0, true); pixel(1, 0, false); /* Rendering cannot mutate the submitted frame. */
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    assert(d.pixel_budget.pending == 1 && (d.pixel_budget.pixels[0] & 0x3fU) == 2);
    assert(epd_pixel_budget_drive(&d.pixel_budget, 1) == EPD_PIXEL_HOLD);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    assert(d.pixel_budget.pending == 2 && epd_pixel_budget_drive(&d.pixel_budget, 0) == EPD_PIXEL_DRIVE_WHITE);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    assert(worker_tick(&d.scan_worker) == ESP_OK && d.pixel_budget.pending == 0);
    /* A failing scan must restore the newest accepted frame immediately. */
    pixel(2, 0, false);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    hook_display = &d; activation_hook = submit_during_activation; fail_command = 0x20;
    assert(worker_tick(&d.scan_worker) == ESP_FAIL);
    assert(d.refresh_mode == EPD_SSD1683_REFRESH_AUTO && d.shadow_valid && !d.scan_submit_ready);
    assert(!d.scan_worker.enabled && memcmp(d.shadow, d.buffer, d.buffer_size) == 0);
    assert(!ram_pixel(0, 2, 0) && !ram_pixel(0, 3, 0));
    epd_scan_worker_deinit(&d.scan_worker); free_budget(&d);
}
static void test_async_cleanup_during_continuous_changes(void) {
    epd_ssd1683_t d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-scan=async") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "drive-cycles=8") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "cleanup-every=3") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "cleanup-policy=fixed") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    for (unsigned frame = 0; frame < 3; ++frame) {
        pixel(frame, 0, false);
        assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
        assert(worker_tick(&d.scan_worker) == ESP_OK);
        assert(d.pixel_budget.pending != 0);
        assert(d.pulse_updates == frame + 1);
        if (frame == 0) {
            /* Duplicate targets and continuation scans do not age cleanup. */
            assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
            assert(worker_tick(&d.scan_worker) == ESP_OK);
            assert(worker_tick(&d.scan_worker) == ESP_OK);
            assert(d.pulse_updates == 1);
        }
    }
    /* The next scan cleans up despite unfinished budgets, using the newest
     * accepted target rather than waiting for all pixels to settle. */
    pixel(3, 0, false);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    capture_count = 0;
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    assert(find(0x22, count(0x22) - 1)->data[0] == 0xf7);
    assert(d.pulse_updates == 0 && d.pixel_budget.pending == 0);
    assert(memcmp(d.shadow, d.buffer, d.buffer_size) == 0);
    for (unsigned x = 0; x < 4; ++x) assert(!ram_pixel(0, x, 0));
    epd_scan_worker_deinit(&d.scan_worker); free_budget(&d);
}
static void test_adaptive_cleanup(void) {
    epd_ssd1683_t d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-scan=async") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "drive-cycles=1") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "cleanup-every=2") == ESP_OK);
    assert(d.pulse_tuning.cleanup_adaptive);
    assert(epd_ssd1683_set_controller_mode(&d, "cleanup-policy=bogus") == ESP_ERR_INVALID_ARG);
    assert(d.pulse_tuning.cleanup_adaptive);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    /* Unrelated changes exceed the fixed interval without native flashes. */
    for (unsigned x = 0; x < 8; ++x) {
        capture_count = 0; pixel(x, 0, false);
        assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
        assert(worker_tick(&d.scan_worker) == ESP_OK);
        assert(d.pulse_updates == x + 1 && d.pixel_budget.peak_drive_cycles == 1);
        assert(!ssd1683_pulse_cleanup_due(&d) || x == 7);
        assert(display_update(0, false)->data[0] == 0xcf);
    }
    assert(ssd1683_pulse_cleanup_due(&d)); /* Bounded age at 4*N. */
    assert(worker_pending); /* Real worker must continue despite zero pending pixels. */
    capture_count = 0;
    assert(worker_tick(&d.scan_worker) == ESP_OK); /* No new presentation required. */
    assert(find(0x22, count(0x22) - 1)->data[0] == 0xf7);
    assert(strstr(last_log, "reason=age") && d.pulse_updates == 0);
    assert(!worker_pending);
    /* The same pixel changing twice reaches its drive limit much sooner. */
    for (unsigned i = 0; i < 2; ++i) {
        pixel(0, 0, i == 0);
        assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
        assert(worker_tick(&d.scan_worker) == ESP_OK);
    }
    assert(d.pixel_budget.peak_drive_cycles == 2 && ssd1683_pulse_cleanup_due(&d));
    assert(worker_pending);
    const char *mode = epd_ssd1683_controller_mode(&d);
    assert(strstr(mode, "cleanup-policy=adaptive") && strstr(mode, "cleanup-peak-cycles=2"));
    assert(strstr(mode, "pending=0")); /* Long mode descriptions retain the tail. */
    capture_count = 0;
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    assert(strstr(last_log, "reason=pixel-drive"));
    assert(d.pixel_budget.peak_drive_cycles == 0 && d.pixel_budget.pending == 0);
    epd_scan_worker_deinit(&d.scan_worker); free_budget(&d);
}
static void test_hold_power_and_fast_ramp(void) {
    epd_ssd1683_t d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "drive-cycles=3") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-power=hold") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-ramp=fast") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-spi-khz=20000") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-spi-khz=20001") == ESP_ERR_INVALID_ARG);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-period=100") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-period=9") == ESP_ERR_INVALID_ARG);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-period=66") == ESP_ERR_INVALID_ARG);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-period=1001") == ESP_ERR_INVALID_ARG);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-period=100x") == ESP_ERR_INVALID_ARG);
    assert(d.scan_period_ms == 100);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK);
    capture_count = 0; pixel(10, 10, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(find(0x0c, 0)->data[3] == 0 && find(0x8c, 0)->data[3] == 0);
    assert(count(0x20) == 5 && display_update_count() == 3);
    assert(find(0x22, 0)->data[0] == 0x80);
    assert(display_update(0, false)->data[0] == 0xcc);
    assert(display_update(1, false)->data[0] == 0x0c && display_update(2, false)->data[0] == 0x0c);
    assert(find(0x22, count(0x22) - 1)->data[0] == 0x03 && !d.pulse_analog_on);
    assert(actual_spi_hz == 20000000);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=auto") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK && actual_spi_hz == 10000000);
    free_budget(&d);
}
static void test_dual_pulse_rate_limit(void) {
    /* The mock panel finishes in 20 ms. Redrives follow BUSY without the
     * new-image delay. Worker timing is covered by epd_scan_worker_test;
     * synchronous mode must pace changed targets here in the adapter.
     */
    for (unsigned async = 0; async < 2; ++async) {
        for (unsigned hold = 0; hold < 2; ++hold) {
            epd_ssd1683_t d = display();
            assert(epd_ssd1683_set_controller_mode(&d, "drive-cycles=3") == ESP_OK);
            assert(epd_ssd1683_set_controller_mode(&d, "pulse-frames=1") == ESP_OK);
            assert(epd_ssd1683_set_controller_mode(&d, "pulse-period=67") == ESP_OK);
            assert(epd_ssd1683_set_controller_mode(&d, hold ? "pulse-power=hold" : "pulse-power=cycle") == ESP_OK);
            if (async)
                assert(epd_ssd1683_set_controller_mode(&d, "pulse-scan=async") == ESP_OK);
            assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
            assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
            if (async) assert(worker_tick(&d.scan_worker) == ESP_OK);
            int64_t previous_activation = 0;
            int64_t previous_frame = 0;
            for (unsigned burst = 0; burst < 2; ++burst) {
                capture_count = 0;
                pixel(0, 0, burst != 0);
                assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
                if (async) {
                    for (unsigned scan = 0; scan < 3; ++scan)
                        assert(worker_tick(&d.scan_worker) == ESP_OK);
                    assert(!worker_pending);
                }
                assert(d.pixel_budget.pending == 0);
                if (!async && previous_frame)
                    assert(d.pulse_last_frame_start - previous_frame >= 67000);
                previous_frame = d.pulse_last_frame_start;
                assert(display_update_count() == 3);
                for (unsigned scan = 0; scan < 3; ++scan) {
                    const uint8_t expected = hold ? (scan == 0 ? 0xcc : 0x0c) : 0xcf;
                    assert(display_update(scan, false)->data[0] == expected);
                    const int64_t activated = display_update(scan, true)->at_us;
                    if (scan != 0)
                        assert(activated - previous_activation >= 20000 &&
                               activated - previous_activation < 67000);
                    previous_activation = activated;
                }
                assert(d.pulse_drive_interval >= 20000 && d.pulse_drive_interval < 67000);
                assert(!d.pulse_analog_on);
            }
            assert(strstr(epd_ssd1683_controller_mode_values(&d), "pulse-period=<67..1000>"));
            if (async) epd_scan_worker_deinit(&d.scan_worker);
            free_budget(&d);
        }
    }
}
static void test_async_sleep_and_resume(void) {
    epd_ssd1683_t d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-scan=async") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-power=hold") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    pixel(0, 0, false);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(worker_tick(&d.scan_worker) == ESP_OK && d.pixel_budget.pending == 1);
    assert(d.pulse_analog_on);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_SET_POWER_SAVE, 1, NULL) == 1);
    assert(!d.powered && !d.scan_submit_ready && !d.scan_worker.enabled && !d.pulse_analog_on);
    capture_count = 0;
    assert(worker_tick(&d.scan_worker) == ESP_OK && capture_count == 0);
    assert(d.pixel_budget.pending == 1); /* Sleeping never silently consumes drive budget. */
    assert(epd_ssd1683_resume(&d) == ESP_OK && d.scan_submit_ready);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    assert(d.pixel_budget.pending == 0 && d.shadow_valid);
    assert(epd_pixel_budget_drive(&d.pixel_budget, 0) == EPD_PIXEL_HOLD);
    epd_scan_worker_deinit(&d.scan_worker); free_budget(&d);
}
static void test_pulse_tuning_preserves_native_idle(void) {
    epd_ssd1683_t d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-scan=async") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-ramp=fast") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-power=hold") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-frames=3") == ESP_OK);
    assert(!d.pulse_needs_cleanup && !d.scan_submit_ready);
    assert(ssd1683_refresh(&d) == ESP_OK && capture_count == 0);
    epd_scan_worker_deinit(&d.scan_worker);
}
static void test_default_async_activation(void) {
    epd_ssd1683_t d = display();
    epd_ssd1683_config_t config = {.panel_variant = d.panel_variant, .spi_clock_hz = 10000000};
    ssd1683_pulse_defaults(&d, &config);
    assert(d.scan_async && d.scan_period_ms == 67 && d.scan_worker.task == NULL);
    assert(d.pulse_power_hold && d.pulse_ramp_fast && d.pulse_spi_clock_hz == 20000000);
    assert(d.pulse_tuning.drive_cycles == 6 && d.pulse_tuning.pulse_frames == 8);
    assert(d.pulse_tuning.waveform_fr == 4 && d.pulse_tuning.cleanup_every == 64);
    assert(d.pulse_tuning.cleanup_adaptive);
    assert(ssd1683_refresh(&d) == ESP_OK && capture_count == 0); /* Auto unchanged. */
    config.startup_pulse = true;
    worker_init_fail = true;
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_ERR_NO_MEM);
    ssd1683_start_default_refresh(&d, &config);
    assert(d.refresh_mode == EPD_SSD1683_REFRESH_AUTO && !d.scan_submit_ready);
    worker_init_fail = false;
    ssd1683_start_default_refresh(&d, &config);
    assert(d.refresh_mode == EPD_SSD1683_REFRESH_PULSE);
    assert(d.scan_worker.task && d.scan_worker.period_ms == 67 && d.scan_submit_ready);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(worker_tick(&d.scan_worker) == ESP_OK); /* Baseline from submitted copy. */
    epd_scan_worker_deinit(&d.scan_worker); free_budget(&d);
    memset(&d, 0, sizeof(d));
    config.panel_variant = EPD_SSD1683_PANEL_WAVESHARE_V2;
    ssd1683_pulse_defaults(&d, &config);
    assert(d.scan_async && d.scan_period_ms == 100);
    assert(!d.pulse_power_hold && !d.pulse_ramp_fast && d.pulse_spi_clock_hz == 10000000);
    assert(d.pulse_tuning.cleanup_every == 64 && d.pulse_tuning.drive_cycles == 3);
    config.panel_variant = EPD_SSD1683_PANEL_LEGACY;
    ssd1683_pulse_defaults(&d, &config);
    assert(!d.scan_async && d.pulse_tuning.cleanup_every == 32);
}
static void test_cleanup_failure_and_capability(void) {
    epd_ssd1683_t d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "cleanup-every=1") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK);
    pixel(0, 0, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    capture_count = 0;
    pixel(0, 0, true);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(d.pulse_updates == 0 && !d.pulse_lut_loaded);
    assert(find(0x22, count(0x22) - 1)->data[0] == 0xf7);
    capture_count = 0;
    pixel(0, 0, false); fail_command = 0xa6;
    assert(ssd1683_refresh(&d) == ESP_FAIL);
    assert(d.pixel_budget.pending == 1 && !d.shadow_valid && !d.controller_ready);
    assert(d.pixel_budget.peak_drive_cycles == 0); /* Failed transfer is not exposure. */
    assert(d.refresh_mode == EPD_SSD1683_REFRESH_AUTO);
    capture_count = 0;
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(d.pixel_budget.pixels == NULL && d.shadow_valid);

    d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK);
    pixel(1, 0, false); no_busy = true; capture_count = 0;
    assert(ssd1683_refresh(&d) == ESP_ERR_TIMEOUT);
    assert(d.pixel_budget.peak_drive_cycles == 0); /* Missing BUSY completion is not exposure. */
    assert(strstr(error_log, "did not assert BUSY") != NULL);
    assert(d.pixel_budget.pending == 1 && d.refresh_mode == EPD_SSD1683_REFRESH_AUTO);
    free_budget(&d);
    d = display();
    d.panel_variant = EPD_SSD1683_PANEL_LEGACY;
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_ERR_INVALID_ARG);
    assert(epd_ssd1683_set_controller_mode(&d, "drive-cycles=3") == ESP_ERR_INVALID_ARG);
    assert(strcmp(epd_ssd1683_controller_mode_values(&d), "refresh=<auto,fast,full>") == 0);
}

static epd_ssd1683_t single_display(void) {
    epd_ssd1683_t d = display();
    ram_height = SSD1683_HEIGHT;
    d.panel_variant = EPD_SSD1683_PANEL_WAVESHARE_V2;
    d.buffer_size = d.shadow_size = SSD1683_BUFFER_BYTES;
    d.line_buffer_size = SSD1683_PANEL_ROW_BYTES;
    d.power_pin = -1;
    const epd_ssd1683_config_t config = {.panel_variant = d.panel_variant, .spi_clock_hz = 10000000};
    ssd1683_pulse_defaults(&d, &config);
    return d;
}
static void single_pixel(unsigned x, unsigned y, bool white) {
    const size_t p = (y / 8U) * SSD1683_WIDTH + x;
    const uint8_t bit = 1U << (y % 8U);
    frame[p] = white ? frame[p] | bit : frame[p] & (uint8_t)~bit;
}
static bool single_ram_pixel(unsigned plane, unsigned x, unsigned y) {
    return (ram[0][plane][(x / 8U) * SSD1683_HEIGHT + y] & (0x80U >> (x % 8U))) != 0;
}
static void test_single_pulse_transport_and_native_restore(void) {
    epd_ssd1683_t d = single_display();
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-scan=sync") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-spi-khz=20000") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(d.pixel_budget.count == 120000 && find(0x24, 0)->size == 15000);
    assert(single_ram_pixel(0, 0, 0) && single_ram_pixel(1, 399, 299));
    capture_count = 0;
    single_pixel(0, 0, false); single_pixel(399, 299, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    for (size_t i = 0; i < capture_count; ++i) assert(captures[i].cmd < 0x80);
    assert(find(0x24, 0)->size == 15000 && find(0x26, 0)->size == 15000);
    assert(count(0x20) == 3 && d.pixel_budget.pending == 0);
    assert(!single_ram_pixel(0, 0, 0) && single_ram_pixel(1, 0, 0));
    assert(!single_ram_pixel(0, 399, 299) && single_ram_pixel(1, 399, 299));
    assert(!single_ram_pixel(0, 1, 0) && !single_ram_pixel(1, 1, 0));
    assert(find(0x21, 0)->data[0] == 0 && find(0x21, 0)->data[1] == 0);
    assert(find(0x2c, 0)->data[0] == 0x30 && find(0x03, 0)->data[0] == 0x17);
    assert(find(0x32, 0)->size == 227 && find(0x32, 0)->data[85] == 0x48);
    assert(find(0x32, 0)->data[127] == 0x88 && actual_spi_hz == 20000000);
    single_pixel(0, 0, true); single_pixel(399, 299, true);
    capture_count = 0;
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(single_ram_pixel(0, 0, 0) && !single_ram_pixel(1, 0, 0));
    assert(single_ram_pixel(0, 399, 299) && !single_ram_pixel(1, 399, 299));
    single_pixel(24, 100, false);
    capture_count = 0;
    assert(ssd1683_refresh(&d) == ESP_OK); /* Clears the previous corner masks. */
    assert(!single_ram_pixel(0, 0, 0) && !single_ram_pixel(1, 0, 0));
    single_pixel(32, 101, false);
    capture_count = 0;
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(find(0x24, 0)->size == 4 && find(0x26, 0)->size == 4);
    assert(find(0x24, 0)->x_start == 3 && find(0x24, 0)->y_start == 100);
    assert(!single_ram_pixel(0, 24, 100) && !single_ram_pixel(1, 24, 100));
    assert(!single_ram_pixel(0, 32, 101) && single_ram_pixel(1, 32, 101));
    capture_count = 0;
    frame[37U * 400U + 10U] ^= 0xf0; /* Four unused rows in the U8g2 buffer. */
    assert(ssd1683_refresh(&d) == ESP_OK && capture_count == 0);
    assert(d.pixel_budget.pending == 0);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=auto") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK && actual_spi_hz == 10000000);
    assert(d.pixel_budget.pixels == NULL && d.pulse_planes == NULL && !d.pulse_lut_loaded);
    assert(find(0x22, count(0x22) - 1)->data[0] == 0xf7);
    assert(!single_ram_pixel(0, 32, 101) && !single_ram_pixel(1, 32, 101));
    capture_count = 0; single_pixel(33, 102, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(find(0x22, 0)->data[0] == 0xff && count(0x32) == 0);
    assert(!single_ram_pixel(0, 33, 102) && !single_ram_pixel(1, 33, 102));
}
static void test_single_async_retarget_and_recovery(void) {
    epd_ssd1683_t d = single_display();
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(d.scan_worker.task && d.scan_submit_ready);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    capture_count = 0; single_pixel(399, 299, false);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(capture_count == 0);
    single_pixel(399, 299, true); single_pixel(398, 298, false);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    assert(d.pixel_budget.pending == 1 && epd_pixel_budget_drive(&d.pixel_budget, 119999) == EPD_PIXEL_DRIVE_BLACK);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    assert(d.pixel_budget.pending == 2 && epd_pixel_budget_drive(&d.pixel_budget, 119999) == EPD_PIXEL_DRIVE_WHITE);
    capture_count = 0;
    hook_display = &d; activation_hook = submit_during_activation; fail_command = 0x20;
    assert(worker_tick(&d.scan_worker) == ESP_FAIL);
    assert(d.refresh_mode == EPD_SSD1683_REFRESH_AUTO && !d.scan_submit_ready && d.shadow_valid);
    assert(memcmp(d.shadow, d.buffer, d.buffer_size) == 0);
    assert(single_ram_pixel(0, 399, 299) && single_ram_pixel(1, 399, 299));
    assert(!single_ram_pixel(0, 398, 298) && !single_ram_pixel(1, 398, 298));
    assert(!single_ram_pixel(0, 3, 0) && !single_ram_pixel(1, 3, 0));
    epd_scan_worker_deinit(&d.scan_worker); free_budget(&d);
}
static void test_control_pin_sleep_retention(void) {
    epd_ssd1683_t d = display();
    sleep_retained = 0;
    assert(ssd1683_configure_pins(&d) == ESP_OK);
    const uint64_t controls = (1ULL << 45) | (1ULL << 46) |
        (1ULL << 47) | (1ULL << 48);
    assert(sleep_retained == (controls | (1ULL << 7)));
    assert(!d.powered);
    d = single_display();
    sleep_retained = 0;
    assert(ssd1683_configure_pins(&d) == ESP_OK);
    assert(sleep_retained == controls && d.powered);
    retention_fail_pin = d.reset_pin;
    assert(ssd1683_configure_pins(&d) == ESP_FAIL);
    retention_fail_pin = -1;
}
static epd_ssd1683_t green_display(void) {
    epd_ssd1683_t d = single_display();
    green_controller = true;
    d.panel_variant = EPD_SSD1683_PANEL_GREEN_STICKER;
    d.busy_level = 0;
    const epd_ssd1683_config_t config = {.panel_variant = d.panel_variant,
        .spi_clock_hz = 10000000};
    ssd1683_pulse_defaults(&d, &config);
    /* Keep transport cases at their explicit calibration values; defaults and
     * startup policy are exercised independently below. */
    d.pulse_tuning.drive_cycles = 3;
    d.pulse_tuning.waveform_fr = 9;
    d.pulse_tuning.cleanup_every = 64;
    d.pulse_tuning.cleanup_adaptive = true;
    d.u8g2.u8x8.display_info = &ssd1683_display_info;
    return d;
}
static void test_green_mask_waveform_and_power(void) {
    epd_ssd1683_t d = green_display();
    assert(d.scan_async && d.pulse_tuning.waveform_fr == 9);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-scan=sync") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(find(0x10, 0)->size == 15000 && find(0x13, 0)->data[14999] == 0xff);
    capture_count = 0;
    single_pixel(0, 0, false); single_pixel(399, 299, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(find(0x10, 0)->data[0] == 0x80 && find(0x10, 0)->data[14999] == 1);
    assert(find(0x13, 0)->data[0] == 0 && find(0x13, 0)->data[14999] == 0);
    assert(find(0x10, 0)->data[1] == 0 && find(0x13, 0)->data[1] == 0);
    assert(count(0x10) == 1 && count(0x13) == 1 && count(0x17) == 3);
    assert(count(0x26) == 0 && count(0x44) == 0 && count(0x32) == 0);
    assert(find(0x50, count(0x50) - 1)->data[0] == 0xd7 && find(0x30, 0)->data[0] == 9);
    for (unsigned i = 0; i < 5; ++i) {
        capture_t *c = find(0x20 + i, 0);
        uint8_t expected[42] = {0};
        if (i == 2 || i == 3) {
            expected[0] = 1;
            expected[2] = (i == 2 ? 0x80 : 0x40) | 8;
            expected[3] = expected[5] = expected[6] = 1;
        }
        assert(c->size == 42 && !memcmp(c->data, expected, 42));
    }
    assert(find(0x82, 0)->data[0] == 0x05); /* UC8276's own VCOM encoding. */
    assert(count(0x2c) == 0 && count(0x3f) == 0 && count(0x03) == 0);
    assert(strstr(epd_ssd1683_controller_mode(&d), "waveform=fastwave-uc8276"));
    assert(d.pixel_budget.pending == 0 && d.pixel_budget.peak_drive_cycles == 3);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-power=hold") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-ramp=fast") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK); /* Changed power settings require a baseline. */
    capture_count = 0;
    single_pixel(0, 0, true); single_pixel(399, 299, true);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(find(0x13, 0)->data[0] == 0x80 && find(0x13, 0)->data[14999] == 1);
    assert(find(0x10, 0)->data[0] == 0 && find(0x10, 0)->data[14999] == 0);
    capture_t *booster = find(0x06, count(0x06) - 1);
    assert(booster->data[0] == 0x16 && booster->data[1] == 0x16);
    assert(booster->data[2] == 0x29);
    assert(count(0x04) == 1 && count(0x12) == 3 && count(0x02) == 1);
    assert(!d.pulse_analog_on && d.pixel_budget.pending == 0);
    capture_count = 0;
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=auto") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(d.pixel_budget.pixels == NULL && d.shadow_valid);
    assert(find(0x22, 0)->data[1] == 0x54 && find(0x17, 0)->data[0] == 0xa5);
}
static void test_green_fastwave_rate_limit(void) {
    for (unsigned async = 0; async < 2; ++async) {
        for (unsigned hold = 0; hold < 2; ++hold) {
            epd_ssd1683_t d = green_display();
            assert(epd_ssd1683_set_controller_mode(&d, async ? "pulse-scan=async" : "pulse-scan=sync") == ESP_OK);
            assert(epd_ssd1683_set_controller_mode(&d, hold ? "pulse-power=hold" : "pulse-power=cycle") == ESP_OK);
            assert(epd_ssd1683_set_controller_mode(&d, "pulse-frames=1") == ESP_OK);
            assert(epd_ssd1683_set_controller_mode(&d, "pulse-period=66") == ESP_ERR_INVALID_ARG);
            assert(epd_ssd1683_set_controller_mode(&d, "pulse-period=67") == ESP_OK);
            assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
            assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
            if (async) assert(worker_tick(&d.scan_worker) == ESP_OK);
            int64_t previous = 0;
            int64_t previous_frame = 0;
            for (unsigned burst = 0; burst < 2; ++burst) {
                capture_count = 0;
                single_pixel(0, 0, burst != 0);
                assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
                if (async)
                    for (unsigned scan = 0; scan < 3; ++scan)
                        assert(worker_tick(&d.scan_worker) == ESP_OK);
                assert(d.pixel_budget.pending == 0 && !d.pulse_analog_on);
                if (!async && previous_frame)
                    assert(d.pulse_last_frame_start - previous_frame >= 67000);
                previous_frame = d.pulse_last_frame_start;
                assert(count(hold ? 0x12 : 0x17) == 3);
                for (unsigned scan = 0; scan < 3; ++scan) {
                    const int64_t now = find(hold ? 0x12 : 0x17, scan)->at_us;
                    if (scan != 0) assert(now - previous >= 20000 && now - previous < 67000);
                    previous = now;
                }
            }
            assert(strstr(epd_ssd1683_controller_mode_values(&d), "pulse-period=<67..1000>"));
            if (async) epd_scan_worker_deinit(&d.scan_worker);
            free_budget(&d);
        }
    }
}
static void test_green_detection_and_native_busy(void) {
    epd_ssd1683_t d = green_display();
    d.panel_variant = EPD_SSD1683_PANEL_UNKNOWN;
    d.busy_level = 1; /* Board configuration starts with SSD1683 polarity. */
    assert(ssd1683_controller_init(&d) == ESP_OK);
    assert(d.panel_variant == EPD_SSD1683_PANEL_GREEN_STICKER && d.busy_level == 0);
    assert(d.scan_async && d.pulse_tuning.waveform_fr == 12);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(time_us >= busy_until && d.shadow_valid);
    single_pixel(0, 0, false); no_busy = true;
    assert(ssd1683_refresh(&d) == ESP_ERR_INVALID_RESPONSE);
    assert(memcmp(d.shadow, d.buffer, d.buffer_size) != 0);
    no_busy = false;
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(memcmp(d.shadow, d.buffer, d.buffer_size) == 0);
}
static void test_green_async_failure_and_idle_cleanup(void) {
    epd_ssd1683_t d = green_display();
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    single_pixel(399, 299, false);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    single_pixel(399, 299, true); /* The worker must consume its copied target. */
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    assert(epd_pixel_budget_drive(&d.pixel_budget, 119999) == EPD_PIXEL_DRIVE_BLACK);
    assert(epd_ssd1683_set_controller_mode(&d, "cleanup-every=1") == ESP_OK);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    assert(d.pixel_budget.pending == 0 && worker_pending);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    assert(!worker_pending && !d.pulse_lut_loaded && d.pixel_budget.peak_drive_cycles == 0);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-power=hold") == ESP_OK);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    single_pixel(0, 0, false);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    fail_command = 0x12;
    assert(worker_tick(&d.scan_worker) == ESP_FAIL);
    assert(d.refresh_mode == EPD_SSD1683_REFRESH_AUTO && d.shadow_valid && !d.scan_submit_ready);
    assert(memcmp(d.shadow, d.buffer, d.buffer_size) == 0);
    epd_scan_worker_deinit(&d.scan_worker); free_budget(&d);
}
static void test_power_state_replaces_queued_pulse_frame(void) {
    epd_ssd1683_t d = display();
    d.lock = xSemaphoreCreateMutexStatic(&d.lock_storage);
    assert(epd_ssd1683_set_controller_mode(&d, "pulse-scan=async") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    pixel(0, 0, false);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    pixel(20, 10, false); /* Final splash replaces the queued app frame. */
    assert(epd_ssd1683_present_power_state(&d) == ESP_OK);
    assert(d.refresh_mode == EPD_SSD1683_REFRESH_PULSE);
    assert(d.power_state_present && !d.scan_submit_ready && !d.scan_worker.enabled);
    assert(d.shadow_valid && !memcmp(d.shadow, d.buffer, d.buffer_size));
    capture_count = 0;
    assert(worker_tick(&d.scan_worker) == ESP_OK && capture_count == 0);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_SET_POWER_SAVE, 1, NULL) == 1);
    assert(epd_ssd1683_resume(&d) == ESP_OK);
    assert(!d.power_state_present && d.scan_submit_ready);
    epd_scan_worker_deinit(&d.scan_worker); free_budget(&d);
}

static void test_green_startup_pulse_defaults(void) {
    epd_ssd1683_config_t config = {.panel_variant = EPD_SSD1683_PANEL_GREEN_STICKER,
        .spi_clock_hz = 10000000, .startup_pulse = true};
    epd_ssd1683_t d = green_display();
    ssd1683_pulse_defaults(&d, &config);
    assert(d.pulse_tuning.drive_cycles == 8 && d.pulse_tuning.pulse_frames == 8);
    assert(d.pulse_tuning.waveform_fr == 12 && d.pulse_tuning.cleanup_every == 8);
    assert(!d.pulse_tuning.cleanup_adaptive && !d.pulse_power_hold);
    assert(d.scan_async && d.scan_period_ms == 67 && d.pulse_ramp_fast);
    assert(d.pulse_spi_clock_hz == 20000000);
    config.startup_pulse = false;
    ssd1683_start_default_refresh(&d, &config);
    assert(d.refresh_mode == EPD_SSD1683_REFRESH_AUTO && d.scan_worker.task == NULL);
    config.startup_pulse = true; worker_init_fail = true;
    ssd1683_start_default_refresh(&d, &config);
    assert(d.refresh_mode == EPD_SSD1683_REFRESH_AUTO && !d.scan_submit_ready);
    worker_init_fail = false;
    ssd1683_start_default_refresh(&d, &config);
    assert(d.refresh_mode == EPD_SSD1683_REFRESH_PULSE && d.scan_worker.task);
    assert(d.scan_worker.period_ms == 67 && d.scan_submit_ready && d.pulse_needs_cleanup);
    assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    assert(worker_tick(&d.scan_worker) == ESP_OK && d.shadow_valid);
    epd_scan_worker_deinit(&d.scan_worker); free_budget(&d);
    d = display(); d.panel_variant = EPD_SSD1683_PANEL_LEGACY;
    ssd1683_start_default_refresh(&d, &config);
    assert(d.refresh_mode == EPD_SSD1683_REFRESH_AUTO && d.scan_worker.task == NULL);
}
static void test_relax_restores_image_and_policy(void) {
    for (unsigned pulse = 0; pulse < 2; ++pulse) {
        epd_ssd1683_t d = display();
        epd_ssd1683_config_t config = {.panel_variant = d.panel_variant, .spi_clock_hz = 10000000};
        ssd1683_pulse_defaults(&d, &config);
        pixel(395, 20, false); pixel(396, 30, false);
        if (pulse) {
            assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
            assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
            assert(worker_tick(&d.scan_worker) == ESP_OK);
            pixel(400, 20, false);
            assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
            assert(d.scan_worker.has_target);
        }
        uint8_t saved[sizeof(frame)];
        memcpy(saved, frame, sizeof(frame));
        const epd_ssd1683_refresh_mode_t mode = d.refresh_mode;
        assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
        assert(!d.scan_submit_ready && !d.scan_worker.has_target);
        assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_ERR_INVALID_STATE);
        assert(epd_ssd1683_set_controller_mode(&d, "refresh=auto") == ESP_ERR_INVALID_STATE);
        capture_count = 0;
        assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_BLACK) == ESP_OK);
        assert(find(0x22, 0)->data[0] == 0xf7);
        assert(!ram_pixel(0, 395, 20) && !ram_pixel(0, 396, 30));
        assert(!ram_pixel(0, 0, 0) && !ram_pixel(0, 791, 271));
        assert(count(0x32) == 0 && count(0xb2) == 0);
        assert(memcmp(frame, saved, sizeof(frame)) == 0);
        capture_count = 0;
        assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_WHITE) == ESP_OK);
        assert(ram_pixel(0, 395, 20) && ram_pixel(0, 396, 30));
        assert(memcmp(frame, saved, sizeof(frame)) == 0);
        /* A normal renderer may update its framebuffer during maintenance;
         * those writes must neither interrupt recovery nor alter the snapshot.
         */
        pixel(10, 10, false); capture_count = 0;
        assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
        if (pulse) assert(worker_tick(&d.scan_worker) == ESP_OK);
        assert(capture_count == 0);
        assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_END) == ESP_OK);
        assert(d.relax_storage == NULL && d.refresh_mode == mode);
        assert(d.shadow_valid && !memcmp(d.shadow, saved, sizeof(frame)));
        assert(!ram_pixel(0, 395, 20) && !ram_pixel(0, 396, 30));
        assert(ram_pixel(0, 10, 10));
        assert(d.pulse_tuning.drive_cycles == 6 && d.scan_period_ms == 67);
        if (pulse) {
            assert(d.scan_submit_ready && d.pixel_budget.pending == 0);
            assert(!d.pulse_needs_cleanup && !d.pulse_lut_loaded);
            /* Recovery used the native LUT. The next changed target must
             * reload Fastwave and its VCOM on both halves before scanning. */
            capture_count = 0;
            assert(ssd1683_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
            assert(worker_tick(&d.scan_worker) == ESP_OK);
            assert(count(0x32) == 1 && count(0xb2) == 1 && d.pulse_lut_loaded);
            assert(find(0x2c, 0)->data[0] == 0x08 && find(0xac, 0)->data[0] == 0x08);
            assert(count(0x3f) == 0 && count(0xbf) == 0);
            epd_scan_worker_deinit(&d.scan_worker);
        }
        free_budget(&d);
    }
}

static void test_relax_errors_and_other_profiles(void) {
    epd_ssd1683_t d = display();
    pixel(25, 25, false);
    assert(epd_ssd1683_relax(&d, (solar_os_display_relax_phase_t)-1) == ESP_ERR_INVALID_ARG);
    assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_END) == ESP_ERR_INVALID_STATE);
    assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    fail_command = 0x24;
    assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_BLACK) == ESP_FAIL);
    assert(!d.controller_ready && !d.shadow_valid);
    assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_END) == ESP_OK);
    assert(d.relax_storage == NULL && d.shadow_valid && !ram_pixel(0, 25, 25));
    capture_count = 0;
    assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    fail_command = 0x24;
    assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_END) == ESP_FAIL);
    assert(d.relax_storage == NULL && !d.shadow_valid && !d.controller_ready);
    d = single_display();
    d.panel_variant = EPD_SSD1683_PANEL_LEGACY;
    single_pixel(10, 10, false);
    assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    capture_count = 0;
    assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_BLACK) == ESP_OK);
    assert(find(0x24, 0)->size == SSD1683_PANEL_ROW_BYTES * SSD1683_HEIGHT);
    capture_count = 0;
    assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_WHITE) == ESP_OK);
    capture_count = 0;
    assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_END) == ESP_OK);
    assert(!d.relax_storage && d.shadow_valid && !single_ram_pixel(0, 10, 10));

    for (unsigned green = 0; green < 2; ++green) {
        d = green ? green_display() : single_display();
        single_pixel(10, 10, false);
        assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
        capture_count = 0;
        assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_BLACK) == ESP_OK);
        capture_count = 0;
        assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_WHITE) == ESP_OK);
        capture_count = 0;
        assert(epd_ssd1683_relax(&d, SOLAR_OS_DISPLAY_RELAX_END) == ESP_OK);
        assert(!d.relax_storage && d.shadow_valid && d.refresh_mode == EPD_SSD1683_REFRESH_AUTO);
        assert(!memcmp(d.shadow, d.buffer, d.buffer_size));
    }
}

static void test_mono_frames(void) {
    const u8g2_cb_t *rotations[] = {U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3};
    /* Odd width, explicit row padding, and set bits beyond the visible tail. */
    const uint8_t bitmap[] = {0xa5, 0xe3, 0xff, 0x81, 0xff, 0xff,
                              0x66, 0xe9, 0xff, 0x18, 0xf8, 0xff};
    for (unsigned dual = 0; dual < 2; ++dual) {
        const unsigned nw = dual ? SSD1683_DUAL_WIDTH : SSD1683_WIDTH;
        const unsigned nh = dual ? SSD1683_DUAL_HEIGHT : SSD1683_HEIGHT;
        for (unsigned rotation = 0; rotation < 4; ++rotation) {
            for (unsigned invert = 0; invert < 2; ++invert) {
                epd_ssd1683_t d = dual ? display() : single_display();
                d.u8g2.cb = rotations[rotation];
                d.u8g2.width = rotation & 1 ? nh : nw;
                d.u8g2.height = rotation & 1 ? nw : nh;
                assert(epd_scan_worker_init(&d.scan_worker, d.buffer_size,
                    ssd1683_scan_process, &d) == ESP_OK);
                epd_scan_worker_enable(&d.scan_worker, true);
                d.scan_submit_ready = 1;
                assert(epd_ssd1683_present_mono_xbm(&d, bitmap, sizeof(bitmap),
                    5, 7, 13, 4, 3, invert) == ESP_OK);
                assert(capture_count == 0); /* Queued, no synchronous SPI. */
                assert(d.scan_worker.stats.submitted == 1);
                assert(!memcmp(d.scan_worker.latest, d.buffer, d.buffer_size));
                for (unsigned ny = 0; ny < nh; ++ny) {
                    for (unsigned nx = 0; nx < nw; ++nx) {
                        unsigned lx = nx, ly = ny;
                        if (rotation == 1) { lx = ny; ly = nw - 1 - nx; }
                        else if (rotation == 2) { lx = nw - 1 - nx; ly = nh - 1 - ny; }
                        else if (rotation == 3) { lx = nh - 1 - ny; ly = nx; }
                        bool black = false;
                        if (lx >= 5 && lx < 18 && ly >= 7 && ly < 11)
                            black = (bitmap[(ly - 7) * 3 + (lx - 5) / 8] &
                                (1U << ((lx - 5) & 7))) != 0;
                        bool white = (d.buffer[(ny / 8) * nw + nx] & (1U << (ny & 7))) != 0;
                        assert(white == (black == (bool)invert));
                    }
                }
                assert(epd_ssd1683_present_mono_xbm(&d, bitmap, sizeof(bitmap),
                    5, 7, 13, 4, 3, !invert) == ESP_OK);
                assert(d.scan_worker.stats.submitted == 2 && d.scan_worker.stats.coalesced == 1);
                memcpy(d.shadow, d.buffer, d.buffer_size);
                assert(epd_ssd1683_present_mono_xbm(&d, bitmap, 1,
                    5, 7, 13, 4, 3, false) == ESP_ERR_INVALID_ARG);
                assert(epd_ssd1683_present_mono_xbm(&d, bitmap, sizeof(bitmap),
                    d.u8g2.width - 1, 0, 13, 4, 3, false) == ESP_ERR_INVALID_ARG);
                assert(epd_ssd1683_present_mono_xbm(&d, bitmap, sizeof(bitmap),
                    0, 0, 13, 4, 0, false) == ESP_ERR_INVALID_ARG);
                assert(!memcmp(d.shadow, d.buffer, d.buffer_size));
                epd_scan_worker_deinit(&d.scan_worker);
                d.scan_submit_ready = 0;
                /* An unchanged native frame uses the ordinary refresh path. */
                assert(epd_ssd1683_present_mono_xbm(&d, bitmap, sizeof(bitmap),
                    5, 7, 13, 4, 3, !invert) == ESP_OK);
                assert(capture_count == 0);
            }
        }
    }
}

int main(void) {
    test_mono_frames();
    test_control_pin_sleep_retention();
    test_native(); test_pulse_and_restore(); test_default_drive_budget();
    test_settled_pixels_replace_mask();
    test_long_phase_voltage_bits();
    test_moving_window_clears_old_mask();
    test_async_retarget_and_recovery(); test_hold_power_and_fast_ramp();
    test_dual_pulse_rate_limit();
    test_async_cleanup_during_continuous_changes();
    test_adaptive_cleanup();
    test_async_sleep_and_resume();
    test_power_state_replaces_queued_pulse_frame();
    test_default_async_activation();
    test_pulse_tuning_preserves_native_idle();
    test_cleanup_failure_and_capability();
    test_single_pulse_transport_and_native_restore();
    test_single_async_retarget_and_recovery();
    test_green_mask_waveform_and_power();
    test_green_fastwave_rate_limit();
    test_green_detection_and_native_busy();
    test_green_async_failure_and_idle_cleanup();
    test_green_startup_pulse_defaults();
    test_relax_restores_image_and_policy();
    test_relax_errors_and_other_profiles();
    puts("SSD1683 single/dual native/pulse transport tests: ok");
    return 0;
}
