#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include "freertos/semphr.h"
static SemaphoreHandle_t watched_panel_lock;
static unsigned panel_lock_takes;
static int tracked_take(SemaphoreHandle_t lock, unsigned timeout)
{
    if (lock == watched_panel_lock) ++panel_lock_takes;
    return xSemaphoreTake(lock, timeout);
}
#define xSemaphoreTake tracked_take
#include "../../src/drivers/epd_ssd1685.c"
#undef xSemaphoreTake

typedef struct {
    uint8_t command;
    size_t length;
    uint8_t data[8064];
} capture_t;
static capture_t captures[512];
static size_t count;
static int dc_level, resets, powers, bus_locks;
static bool stuck_busy, held_cs;
static int fail_after = -1;
static esp_err_t bus_error;
static int64_t time_us, busy_until;
static bool missing_busy, worker_init_fail, worker_pending, worker_snapshot_valid;
static uint8_t fail_command;
static uint32_t last_spi_hz;

/* Hardware adapter is stepped deterministically here; the real shared worker
 * is independently exercised with pthreads by epd_scan_worker_test. */
esp_err_t epd_scan_worker_init(epd_scan_worker_t *w, size_t n, epd_scan_process_fn fn, void *ctx)
{
    if (worker_init_fail) return ESP_ERR_NO_MEM;
    memset(w, 0, sizeof(*w));
    w->latest = calloc(1, n); w->snapshot = calloc(1, n);
    assert(w->latest && w->snapshot);
    w->lock = xSemaphoreCreateMutexStatic(&w->lock_storage);
    w->task = (void *)1; w->frame_size = n; w->process = fn; w->context = ctx;
    worker_pending = worker_snapshot_valid = false;
    return ESP_OK;
}
void epd_scan_worker_enable(epd_scan_worker_t *w, bool enabled)
{
    if (!w->task) return;
    if (w->enabled != enabled) { ++w->generation; worker_snapshot_valid = false; }
    w->enabled = enabled;
    if (!enabled) w->has_target = false;
}
esp_err_t epd_scan_worker_submit(epd_scan_worker_t *w, const uint8_t *frame)
{
    if (!w->task || !w->enabled) return ESP_ERR_INVALID_STATE;
    if (w->has_target) ++w->stats.coalesced;
    memcpy(w->latest, frame, w->frame_size); w->has_target = true;
    ++w->stats.submitted; return ESP_OK;
}
esp_err_t epd_scan_worker_set_period(epd_scan_worker_t *w, unsigned ms)
{
    if (!w->task) return ESP_ERR_INVALID_STATE;
    if (ms < 10 || ms > 1000) return ESP_ERR_INVALID_ARG;
    w->period_ms = ms; return ESP_OK;
}
epd_scan_stats_t epd_scan_worker_stats(epd_scan_worker_t *w) { return w->stats; }
void epd_scan_worker_deinit(epd_scan_worker_t *w)
{
    free(w->latest); free(w->snapshot); w->latest = w->snapshot = NULL; w->task = NULL;
}
static esp_err_t worker_tick(epd_scan_worker_t *w)
{
    bool fresh = w->has_target && (!worker_snapshot_valid ||
        memcmp(w->latest, w->snapshot, w->frame_size) != 0);
    bool scanned = false;
    if (fresh) memcpy(w->snapshot, w->latest, w->frame_size);
    w->has_target = false;
    esp_err_t err = w->process(w->context, w->snapshot, fresh, &worker_pending, &scanned);
    if (fresh && err == ESP_OK) { ++w->stats.frames; worker_snapshot_valid = true; }
    if (scanned) ++w->stats.scans;
    return err;
}
static uint8_t frame[8064], shadow[8064], line[21];

esp_err_t gpio_config(const gpio_config_t *config) { assert(config); return ESP_OK; }
esp_err_t gpio_reset_pin(gpio_num_t pin) { (void)pin; return ESP_OK; }
esp_err_t spi_bus_remove_device(spi_device_handle_t device) { assert(device); return ESP_OK; }
esp_err_t solar_os_bus_spi_add_device(const char *name,
                                     const spi_device_interface_config_t *config,
                                     spi_device_handle_t *device)
{
    assert(strcmp(name, "spi0") == 0 && config->spics_io_num == 10);
    if (bus_error) return bus_error;
    *device = (void *)1; return ESP_OK;
}

void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0 && !bus_locks); /* never block SD during panel BUSY */
    time_us += ticks * SOLAR_OS_EPD_TEST_TICK_MS * 1000LL;
}
int64_t esp_timer_get_time(void) { return time_us; }
int gpio_get_level(gpio_num_t pin) { (void)pin; return stuck_busy || time_us < busy_until; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    if (pin == 9) dc_level = level;
    if (pin == 46 && !level) resets++;
    if (pin == 7 && level) powers++;
    return ESP_OK;
}
esp_err_t spi_device_acquire_bus(spi_device_handle_t device, unsigned wait)
{
    assert(device && wait == portMAX_DELAY && !bus_locks);
    bus_locks++;
    return ESP_OK;
}
void spi_device_release_bus(spi_device_handle_t device)
{
    assert(device && bus_locks == 1 && !held_cs);
    bus_locks--;
}
esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *t)
{
    assert(device);
    last_spi_hz = t->override_freq_hz;
    if (fail_after == 0) { fail_after = -1; return ESP_FAIL; }
    if (fail_after > 0) fail_after--;
    if (!t->length) { held_cs = false; return ESP_OK; }
    const uint8_t *data = t->flags & SPI_TRANS_USE_TXDATA ? t->tx_data : t->tx_buffer;
    const size_t length = t->length / 8;
    if (!dc_level) {
        assert(length == 1 && count < 512 && !held_cs);
        if (data[0] == fail_command) { fail_command = 0; return ESP_FAIL; }
        if (data[0] == 0x20 && count && captures[count - 1].command == 0x22 &&
            captures[count - 1].length == 1 && !missing_busy) {
            uint8_t mode = captures[count - 1].data[0];
            if (mode == 0x04 || mode == 0xc4 || mode == 0xc7) busy_until = time_us + 5000;
        }
        captures[count++] = (capture_t){.command = data[0]};
    } else {
        assert(count);
        capture_t *c = &captures[count - 1];
        assert(c->length + length <= sizeof(c->data));
        memcpy(c->data + c->length, data, length);
        c->length += length;
    }
    held_cs = (t->flags & SPI_TRANS_CS_KEEP_ACTIVE) != 0;
    if (held_cs) assert(bus_locks == 1);
    return ESP_OK;
}

static epd_ssd1685_t make_display(void)
{
    memset(frame, 0xff, sizeof(frame)); /* SolarOS 1 = white. */
    memset(shadow, 0xff, sizeof(shadow));
    count = 0;
    return (epd_ssd1685_t){.spi = (void *)1, .buffer = frame, .shadow = shadow,
        .line_buffer = line, .dc_pin = 9, .reset_pin = 46,
        .busy_pin = 3, .power_pin = 7,
        .pulse_tuning = {.drive_cycles = 3, .pulse_frames = 10, .waveform_fr = 5,
                         .cleanup_every = 5, .cleanup_adaptive = true},
        .pulse_power_hold = true, .scan_period_ms = 100, .pulse_spi_clock_hz = 10000000};
}
static capture_t *find(uint8_t cmd, unsigned occurrence)
{
    for (unsigned i = 0; i < count; ++i)
        if (captures[i].command == cmd && occurrence-- == 0) return &captures[i];
    assert(!"missing command"); return NULL;
}
static bool has(uint8_t cmd)
{
    for (unsigned i = 0; i < count; ++i) if (captures[i].command == cmd) return true;
    return false;
}
static void expect(uint8_t cmd, const uint8_t *data, size_t len)
{
    capture_t *c = find(cmd, 0);
    assert(c->length == len && memcmp(c->data, data, len) == 0);
}

static void test_full_and_partial(void)
{
    epd_ssd1685_t d = make_display();
    frame[0] &= (uint8_t)~1U;
    frame[167] &= (uint8_t)~1U;
    frame[8063] &= (uint8_t)~0x80U;
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    assert(d.shadow_valid && resets == 1 && powers == 1 && !d.analog_on);
    expect(0x01, (uint8_t[]){0x7f, 1, 0}, 3);
    expect(0x44, (uint8_t[]){1, 21}, 2); /* S8..S175, not S0..S167 */
    expect(0x45, (uint8_t[]){0, 0, 0x7f, 1}, 4);
    expect(0x4e, (uint8_t[]){1}, 1);
    expect(0x18, (uint8_t[]){0x80}, 1);
    expect(0x22, (uint8_t[]){0xf7}, 1);
    assert(find(0x21, 0)->data[1] == 0); /* 200-source address mode */
    capture_t *new = find(0x24, 0);
    assert(new->length == 8064 && new->data[0] == 0x7f && new->data[20] == 0xfe);
    assert(new->data[8063] == 0xfe);
    assert(memcmp(find(0x26, 1)->data, new->data, 8064) == 0);
    assert(memcmp(frame, shadow, 8064) == 0);
    count = 0;
    assert(epd_ssd1685_refresh(&d) == ESP_OK && count == 0);

    frame[0] = 0xff; /* erase the top-left ink */
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    expect(0x21, (uint8_t[]){0, 0}, 2);
    expect(0x22, (uint8_t[]){0xdc}, 1);
    assert(find(0x26, 0)->data[0] == 0x7f && find(0x24, 0)->data[0] == 0xff);
    assert(find(0x26, 1)->data[0] == 0xff && d.partial_count == 1 && d.analog_on);
    for (unsigned i = 2; i <= 6; ++i) {
        count = 0; frame[0] ^= 1;
        assert(epd_ssd1685_refresh(&d) == ESP_OK);
        expect(0x22, (uint8_t[]){i == 6 ? 0xf7 : 0xdc}, 1);
        assert(d.partial_count == (i == 6 ? 0 : i));
    }
    count = 0;
    assert(epd_ssd1685_set_controller_mode(&d, "refresh=full") == ESP_OK);
    assert(epd_ssd1685_refresh(&d) == ESP_OK && has(0x20));
    assert(epd_ssd1685_set_controller_mode(&d, "invalid") == ESP_ERR_NOT_FOUND);
    assert(epd_ssd1685_set_controller_mode(&d, "refresh=partial") == ESP_OK);
    count = 0; frame[0] ^= 1; d.partial_count = 9;
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    expect(0x22, (uint8_t[]){0xdc}, 1);
}

static void test_fastwave_lut(void)
{
    uint8_t lut[227];
    /* Independent register offsets from SSD1685 figure 6-6, not SSD1683's
     * 42-byte blocks. Exercise every exposed timing and polarity setting.
     */
    for (unsigned frames = 1; frames <= 32; ++frames)
        for (unsigned fr = 1; fr <= 15; ++fr)
            for (unsigned invert = 0; invert <= 1; ++invert) {
                assert(solar_os_ssd1685_waveform_build(lut, frames, fr, invert));
                const uint8_t hold[7] = {1, 0, frames, 1, 0, 1, 1};
                assert(memcmp(lut, hold, 7) == 0 && memcmp(lut + 56, hold, 7) == 0);
                assert(lut[114] == ((invert ? 0x40 : 0x80) | frames));
                assert(lut[170] == ((invert ? 0x80 : 0x40) | frames));
                assert(lut[224] == fr && lut[225] == 0 && lut[226] == 0);
                for (unsigned block = 0; block < 4; ++block)
                    for (unsigned i = 7; i < 56; ++i) assert(lut[block * 56 + i] == 0);
            }
    assert(!solar_os_ssd1685_waveform_build(lut, 0, 5, false));
    assert(!solar_os_ssd1685_waveform_build(lut, 33, 5, false));
    assert(!solar_os_ssd1685_waveform_build(lut, 10, 0, false));
    assert(!solar_os_ssd1685_waveform_build(lut, 10, 16, false));
    assert(!solar_os_ssd1685_waveform_build(NULL, 10, 5, false));
}


static void release_pulse(epd_ssd1685_t *d)
{
    scan_disable(d); epd_scan_worker_deinit(&d->scan_worker); free_budget(d);
}

static void test_pulse_sync_and_cleanup(void)
{
    epd_ssd1685_t d = make_display();
    frame[1] &= (uint8_t)~1U;
    assert(epd_ssd1685_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(epd_ssd1685_refresh(&d) == ESP_OK && !has(0x32));
    expect(0x22, (uint8_t[]){0xf7}, 1);
    count = 0; frame[0] ^= 1;
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    assert(find(0x32, 0)->length == 227);
    assert(find(0x32, 0)->data[114] == 0x8a && find(0x32, 0)->data[170] == 0x4a);
    assert(find(0x22, 1)->data[0] == 0xc4 && find(0x22, 2)->data[0] == 0x04);
    assert(find(0x22, 3)->data[0] == 0x04 && find(0x22, 4)->data[0] == 0x03);
    expect(0x2c, (uint8_t[]){0x08}, 1);
    assert(find(0x26, 0)->data[0] == 0x7f && find(0x24, 0)->data[0] == 0);
    assert(d.pixel_budget.pending == 0 && d.pixel_budget.drive_history[0] == 3);
    assert(d.pixel_budget.drive_history[1] == 0 && !d.analog_on);
    count = 0;
    assert(epd_ssd1685_refresh(&d) == ESP_OK && !count);
    int64_t first = d.pulse_last_frame_start;
    count = 0; frame[0] ^= 1; frame[2] ^= 1;
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    assert(d.pulse_last_frame_start - first >= 100000);
    assert(find(0x26, 0)->data[0] == 0x5f && find(0x24, 0)->data[0] == 0x80);
    assert(d.pixel_budget.drive_history[0] == 6 && d.pixel_budget.drive_history[2] == 3);

    assert(epd_ssd1685_set_controller_mode(&d, "cleanup-policy=fixed") == ESP_OK);
    assert(epd_ssd1685_set_controller_mode(&d, "cleanup-every=2") == ESP_OK);
    count = 0;
    assert(epd_ssd1685_refresh(&d) == ESP_OK && has(0x12)); /* lower limit applies while idle */
    assert(!d.pulse_updates);
    for (unsigned n = 1; n <= 2; ++n) {
        count = 0; frame[0] ^= 1;
        assert(epd_ssd1685_refresh(&d) == ESP_OK);
        assert(d.pulse_updates == (n == 2 ? 0 : 1));
        assert(has(0x12) == (n == 2));
    }

    assert(epd_ssd1685_set_controller_mode(&d, "cleanup-policy=adaptive") == ESP_OK);
    assert(epd_ssd1685_set_controller_mode(&d, "drive-cycles=2") == ESP_OK);
    count = 0; frame[0] ^= 1;
    assert(epd_ssd1685_refresh(&d) == ESP_OK && !has(0x12));
    count = 0; frame[3] ^= 1;
    assert(epd_ssd1685_refresh(&d) == ESP_OK && !has(0x12));
    assert(d.pulse_updates == 2 && d.pixel_budget.peak_drive_cycles == 2);
    count = 0; frame[0] ^= 1;
    assert(epd_ssd1685_refresh(&d) == ESP_OK && has(0x12));
    assert(d.pulse_updates == 0 && d.pixel_budget.peak_drive_cycles == 0);

    assert(epd_ssd1685_set_controller_mode(&d, "pulse-frames=6") == ESP_OK);
    assert(epd_ssd1685_set_controller_mode(&d, "waveform-fr=12") == ESP_OK);
    assert(epd_ssd1685_set_controller_mode(&d, "invert=true") == ESP_OK);
    assert(epd_ssd1685_set_controller_mode(&d, "pulse-ramp=fast") == ESP_OK);
    assert(epd_ssd1685_set_controller_mode(&d, "pulse-spi-khz=500") == ESP_OK);
    count = 0; assert(epd_ssd1685_refresh(&d) == ESP_OK);
    expect(0x22, (uint8_t[]){0xf7}, 1);
    count = 0; frame[4] ^= 1;
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    assert(find(0x32, 0)->data[114] == 0x46 && find(0x32, 0)->data[170] == 0x86);
    expect(0x0c, (uint8_t[]){0x8b,0x9c,0x96,0}, 4);
    assert(last_spi_hz == 500000);
    assert(strstr(epd_ssd1685_controller_mode(&d), "cleanup-policy=adaptive"));
    assert(strstr(epd_ssd1685_controller_mode_values(&d), "pulse-period=<67..1000>"));
    const char *invalid[] = {"pulse-period=66", "pulse-period=1001", "pulse-spi-khz=10001",
        "pulse-spi-khz=99", "drive-cycles=0", "cleanup-every=129", "cleanup-policy=bad",
        "pulse-frames=-1", "pulse-frames=", "pulse-frames=33", "waveform-fr=13"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i)
        assert(epd_ssd1685_set_controller_mode(&d, invalid[i]) == ESP_ERR_INVALID_ARG);
    assert(d.pulse_tuning.pulse_frames == 6 && d.pulse_tuning.waveform_fr == 12);
    assert(epd_ssd1685_set_controller_mode(&d, "pulse-power=cycle") == ESP_OK);
    count = 0; assert(epd_ssd1685_refresh(&d) == ESP_OK);
    count = 0; frame[5] ^= 1;
    assert(epd_ssd1685_refresh(&d) == ESP_OK && !d.analog_on);
    assert(find(0x22, 1)->data[0] == 0xc7 && find(0x22, 2)->data[0] == 0xc7);
    assert(!has(0x03) && !has(0x04)); /* never override factory source/gate voltages */
    assert(epd_ssd1685_set_controller_mode(&d, "waveform=otp") == ESP_OK);
    count = 0;
    assert(epd_ssd1685_refresh(&d) == ESP_OK && has(0x12) && !has(0x32));
    assert(!d.pixel_budget.pixels && !d.pulse_planes);
    expect(0x22, (uint8_t[]){0xf7}, 1);
    release_pulse(&d);
}

static void test_native_cleanup_tuning(void)
{
    epd_ssd1685_t d = make_display();
    assert(epd_ssd1685_set_controller_mode(&d, "cleanup-every=2") == ESP_OK);
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    for (unsigned i = 1; i <= 3; ++i) {
        count = 0; frame[0] ^= 1;
        assert(epd_ssd1685_refresh(&d) == ESP_OK);
        expect(0x22, (uint8_t[]){i == 3 ? 0xf7 : 0xdc}, 1);
    }
}

static void test_pulse_async_retarget_and_recovery(void)
{
    epd_ssd1685_t d = make_display();
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    assert(epd_ssd1685_set_controller_mode(&d, "pulse-scan=async") == ESP_OK);
    assert(epd_ssd1685_set_controller_mode(&d, "cleanup-every=64") == ESP_OK);
    assert(epd_ssd1685_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    count = 0; assert(epd_ssd1685_refresh(&d) == ESP_OK && !count); /* just enqueue */
    assert(worker_tick(&d.scan_worker) == ESP_OK && !worker_pending && !has(0x32));
    frame[0] ^= 1; count = 0;
    assert(epd_ssd1685_refresh(&d) == ESP_OK && !count);
    assert(worker_tick(&d.scan_worker) == ESP_OK && worker_pending);
    assert(d.pixel_budget.drive_history[0] == 1 && d.pixel_budget.pending == 1);
    /* Repeated image submissions do not refill the remaining two activations. */
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    count = 0; assert(worker_tick(&d.scan_worker) == ESP_OK && worker_pending && !has(0x24));
    count = 0; assert(worker_tick(&d.scan_worker) == ESP_OK && !worker_pending);
    assert(d.pixel_budget.drive_history[0] == 3 && d.pulse_updates == 1);

    frame[0] ^= 1; assert(epd_ssd1685_refresh(&d) == ESP_OK);
    count = 0; assert(worker_tick(&d.scan_worker) == ESP_OK && worker_pending);
    frame[0] ^= 1; assert(epd_ssd1685_refresh(&d) == ESP_OK);
    frame[7] ^= 1; assert(epd_ssd1685_refresh(&d) == ESP_OK); /* latest frame wins */
    assert(d.scan_worker.stats.coalesced);
    count = 0; assert(worker_tick(&d.scan_worker) == ESP_OK);
    assert(epd_pixel_budget_drive(&d.pixel_budget, 0) == EPD_PIXEL_DRIVE_BLACK);
    assert(d.pixel_budget.pending == 2);

    /* A newer queued target survives failure while an older image is settling. */
    frame[8] ^= 1; assert(epd_ssd1685_refresh(&d) == ESP_OK);
    fail_command = 0x20; count = 0;
    bool pending, scanned;
    assert(scan_process(&d, d.scan_worker.snapshot, false, &pending, &scanned) == ESP_FAIL);
    assert(!pending && !scanned && !d.scan_worker.enabled);
    assert(d.refresh_mode == EPD_SSD1685_REFRESH_AUTO && d.shadow_valid);
    assert(memcmp(d.shadow, frame, 8064) == 0 && has(0x12));
    assert(d.pixel_budget.drive_history[7] == 1); /* failed scan consumed no budget */

    assert(epd_ssd1685_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    count = 0; assert(epd_ssd1685_refresh(&d) == ESP_OK);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    frame[9] ^= 1; assert(epd_ssd1685_refresh(&d) == ESP_OK);
    count = 0; assert(worker_tick(&d.scan_worker) == ESP_OK && worker_pending);
    assert(sleep_panel(&d) == ESP_OK && !d.scan_worker.enabled);
    count = 0;
    assert(epd_ssd1685_refresh(&d) == ESP_ERR_INVALID_STATE);
    assert(worker_tick(&d.scan_worker) == ESP_OK && !count);
    assert(epd_ssd1685_resume(&d) == ESP_OK);
    count = 0; assert(epd_ssd1685_refresh(&d) == ESP_OK);
    assert(worker_tick(&d.scan_worker) == ESP_OK && !has(0x32));
    release_pulse(&d);
}

static void test_pulse_busy_failure(void)
{
    epd_ssd1685_t d = make_display();
    assert(epd_ssd1685_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    count = 0; frame[0] ^= 1; missing_busy = true;
    assert(epd_ssd1685_refresh(&d) == ESP_ERR_TIMEOUT);
    assert(!d.shadow_valid && !d.controller_ready && d.pixel_budget.pending == 1);
    assert(d.pixel_budget.peak_drive_cycles == 0); /* no false optical completion */
    missing_busy = false; count = 0;
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    expect(0x22, (uint8_t[]){0xf7}, 1);
    release_pulse(&d);
}

static void test_pulse_selection_failure(void)
{
    epd_ssd1685_t d = make_display();
    d.scan_async = true;
    worker_init_fail = true;
    assert(epd_ssd1685_set_controller_mode(&d, "refresh=pulse") == ESP_ERR_NO_MEM);
    assert(d.refresh_mode == EPD_SSD1685_REFRESH_AUTO && !d.scan_submit_ready);
    assert(!d.pixel_budget.pixels && !d.pulse_planes && !d.scan_worker.task);
    worker_init_fail = false;
    assert(epd_ssd1685_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(d.scan_worker.task && d.pixel_budget.pixels);
    release_pulse(&d);
    d.shadow = NULL;
    assert(epd_ssd1685_set_controller_mode(&d, "refresh=pulse") == ESP_ERR_NO_MEM);
}

static void test_failures_and_sleep(void)
{
    epd_ssd1685_t d = make_display();
    d.controller_ready = true; d.shadow_valid = true; d.powered = true;
    frame[0] &= (uint8_t)~1U; stuck_busy = true;
    assert(epd_ssd1685_refresh(&d) == ESP_ERR_TIMEOUT);
    assert(!d.controller_ready && !d.shadow_valid && shadow[0] == 0xff);
    assert(d.last_error == ESP_ERR_TIMEOUT);
    stuck_busy = false; count = 0;
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    expect(0x22, (uint8_t[]){0xf7}, 1);
    count = 0; frame[0] ^= 1; fail_after = 30; /* mid-plane burst */
    assert(epd_ssd1685_refresh(&d) == ESP_FAIL);
    assert(!d.shadow_valid && !d.controller_ready && !bus_locks && !held_cs);
    count = 0;
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    expect(0x22, (uint8_t[]){0xf7}, 1);
    count = 0; frame[0] ^= 1;
    assert(epd_ssd1685_refresh(&d) == ESP_OK && d.analog_on);
    count = 0;
    assert(sleep_panel(&d) == ESP_OK);
    expect(0x22, (uint8_t[]){0x83}, 1);
    expect(0x10, (uint8_t[]){1}, 1);
    assert(!d.controller_ready && !d.shadow_valid && !d.powered);
    count = 0;
    assert(epd_ssd1685_refresh(&d) == ESP_ERR_INVALID_STATE && !count);
    assert(epd_ssd1685_resume(&d) == ESP_OK);
    assert(epd_ssd1685_refresh(&d) == ESP_OK && has(0x12));
    expect(0x22, (uint8_t[]){0xf7}, 1);
    d.shadow = NULL; count = 0; /* full refresh fallback without shadow memory */
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    expect(0x22, (uint8_t[]){0xf7}, 1);
}

static void test_raster_async_snapshot(void)
{
    epd_ssd1685_t d = {0};
    const epd_ssd1685_config_t config = {.spi_bus = "spi0", .cs_pin = 10, .dc_pin = 9,
        .reset_pin = 46, .busy_pin = 3, .power_pin = 7, .spi_clock_hz = 10000000,
        .rotation = U8G2_R3, .panel = EPD_SSD1685_PANEL_GDEY029T71H};
    count = 0;
    assert(epd_ssd1685_init(&d, &config) == ESP_OK);
    assert(d.refresh_mode == EPD_SSD1685_REFRESH_PULSE && d.scan_submit_ready);
    assert(d.scan_worker.task && d.scan_worker.enabled && d.scan_async);
    assert(d.pulse_tuning.cleanup_every == 64 && d.pulse_tuning.cleanup_adaptive);
    assert(d.pulse_tuning.drive_cycles == 3 && d.scan_period_ms == 100);
    assert(d.scan_worker.period_ms == 100);
    watched_panel_lock = d.lock;
    uint8_t bitmap[160 / 8 * 144] = {1}; /* Game Boy-sized MONO1 frame */
    uint8_t snapshot[EPD_SSD1685_BUFFER_BYTES];
    count = 0;
    const unsigned takes = panel_lock_takes;
    assert(epd_ssd1685_present_mono_xbm(&d, bitmap, sizeof(bitmap), 112, 12, 160, 144, 20, false) == ESP_OK);
    assert(!count && d.scan_worker.has_target && panel_lock_takes == takes);
    memcpy(snapshot, d.buffer, sizeof(snapshot));
    /* Composition and enqueue copy the source. Later drawing cannot change
     * the image the background worker is about to present. */
    memset(d.buffer, 0, EPD_SSD1685_BUFFER_BYTES);
    bitmap[0] = 0;
    assert(worker_tick(&d.scan_worker) == ESP_OK && !worker_pending);
    assert(memcmp(d.shadow, snapshot, sizeof(snapshot)) == 0);
    count = 0;
    assert(epd_ssd1685_present_mono_xbm(&d, bitmap, sizeof(bitmap), 112, 12, 160, 144, 20, false) == ESP_OK);
    assert(!count);
    assert(worker_tick(&d.scan_worker) == ESP_OK && worker_pending);
    assert(d.pixel_budget.pending == 1);
    assert(epd_pixel_budget_drive(&d.pixel_budget, 271U * 168U + 12U) == EPD_PIXEL_DRIVE_WHITE);
    assert(sleep_panel(&d) == ESP_OK);
    memcpy(snapshot, d.buffer, sizeof(snapshot));
    count = 0;
    assert(epd_ssd1685_present_mono_xbm(&d, bitmap, sizeof(bitmap), 112, 12, 160, 144, 20, false) == ESP_ERR_INVALID_STATE);
    assert(!count && memcmp(d.buffer, snapshot, sizeof(snapshot)) == 0);
    assert(epd_ssd1685_resume(&d) == ESP_OK);
    count = 0;
    assert(epd_ssd1685_present_mono_xbm(&d, bitmap, sizeof(bitmap), 112, 12, 160, 144, 20, false) == ESP_OK);
    assert(!count && worker_tick(&d.scan_worker) == ESP_OK && !has(0x32));
    expect(0x22, (uint8_t[]){0xf7}, 1);
    watched_panel_lock = NULL;
    const epd_pulse_tuning_t tuning = d.pulse_tuning;
    memcpy(snapshot, d.buffer, sizeof(snapshot));
    assert(epd_ssd1685_relax(&d, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    assert(!d.scan_worker.enabled && !d.scan_submit_ready && d.relax.storage);
    count = 0;
    assert(worker_tick(&d.scan_worker) == ESP_OK && !count);
    assert(epd_ssd1685_set_controller_mode(&d, "cleanup-every=1") == ESP_ERR_INVALID_STATE);
    assert(epd_ssd1685_relax(&d, SOLAR_OS_DISPLAY_RELAX_BLACK) == ESP_OK);
    expect(0x22, (uint8_t[]){0xf7}, 1);
    count = 0;
    assert(epd_ssd1685_relax(&d, SOLAR_OS_DISPLAY_RELAX_WHITE) == ESP_OK);
    count = 0;
    assert(epd_ssd1685_relax(&d, SOLAR_OS_DISPLAY_RELAX_END) == ESP_OK);
    assert(d.scan_submit_ready && d.scan_worker.enabled && !d.relax.storage);
    assert(!d.pixel_budget.pending && !d.pixel_budget.peak_drive_cycles && !d.pulse_updates);
    assert(!d.pulse_needs_cleanup && d.refresh_mode == EPD_SSD1685_REFRESH_PULSE);
    assert(memcmp(&d.pulse_tuning, &tuning, sizeof(tuning)) == 0);
    assert(memcmp(d.buffer, snapshot, sizeof(snapshot)) == 0 && d.shadow_valid);
    epd_ssd1685_deinit(&d);
    assert(!d.spi && !d.buffer && !d.scan_worker.task && !d.pixel_budget.pixels);
}

static void test_power_state_replaces_queued_scan(void)
{
    epd_ssd1685_t d = {0};
    const epd_ssd1685_config_t config = {.spi_bus = "spi0", .cs_pin = 10, .dc_pin = 9,
        .reset_pin = 46, .busy_pin = 3, .power_pin = 7, .spi_clock_hz = 10000000,
        .rotation = U8G2_R3, .panel = EPD_SSD1685_PANEL_GDEY029T71H};
    count = 0;
    assert(epd_ssd1685_init(&d, &config) == ESP_OK);
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        memset(d.buffer, 0x55, EPD_SSD1685_BUFFER_BYTES);
        assert(epd_ssd1685_refresh(&d) == ESP_OK && d.scan_worker.has_target);
        memset(d.buffer, 0xaa, EPD_SSD1685_BUFFER_BYTES);
        count = 0;
        assert(epd_ssd1685_present_power_state(&d) == ESP_OK);
        assert(d.power_state_present && !d.scan_worker.enabled && !d.scan_submit_ready);
        assert(!d.scan_worker.has_target && d.refresh_mode == EPD_SSD1685_REFRESH_PULSE);
        expect(0x22, (uint8_t[]){0xf7}, 1);
        assert(d.shadow_valid && !memcmp(d.shadow, d.buffer, EPD_SSD1685_BUFFER_BYTES));
        count = 0;
        assert(worker_tick(&d.scan_worker) == ESP_OK && !count);
        u8g2_SetPowerSave(&d.u8g2, 1);
        assert(d.suspended && !d.scan_submit_ready);
        u8g2_SetPowerSave(&d.u8g2, 0);
        assert(!d.power_state_present && !d.suspended && d.scan_submit_ready);
    }
    fail_command = 0x24;
    assert(epd_ssd1685_present_power_state(&d) == ESP_FAIL);
    assert(d.last_error == ESP_FAIL && !d.scan_submit_ready);
    fail_command = 0;
    assert(epd_ssd1685_resume(&d) == ESP_OK && !d.power_state_present && d.scan_submit_ready);
    epd_ssd1685_deinit(&d);
}

static void test_init_rotation_and_cleanup(void)
{
    const u8g2_cb_t *rotations[] = {U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3};
    epd_ssd1685_config_t c = {.spi_bus = "spi0", .cs_pin = 10, .dc_pin = 9,
        .reset_pin = 46, .busy_pin = 3, .power_pin = 7, .spi_clock_hz = 10000000,
        .rotation = U8G2_R0, .panel = EPD_SSD1685_PANEL_GDEY029T71H};
    epd_ssd1685_t d = {0};
    for (unsigned r = 0; r < 4; ++r) {
        count = 0; c.rotation = rotations[r];
        assert(epd_ssd1685_init(&d, &c) == ESP_OK);
        assert(epd_ssd1685_set_controller_mode(&d, "refresh=auto") == ESP_OK);
        assert(u8g2_GetDisplayWidth(&d.u8g2) == (r % 2 ? 384 : 168));
        assert(u8g2_GetDisplayHeight(&d.u8g2) == (r % 2 ? 168 : 384));
        u8g2_SetDrawColor(&d.u8g2, 0);
        u8g2_DrawPixel(&d.u8g2, 0, 0);
        u8g2_DrawPixel(&d.u8g2, u8g2_GetDisplayWidth(&d.u8g2) - 1,
                       u8g2_GetDisplayHeight(&d.u8g2) - 1);
        unsigned ink = 0;
        for (unsigned i = 0; i < 8064; ++i) ink += __builtin_popcount((unsigned)(uint8_t)~d.buffer[i]);
        assert(ink == 2);
        /* Compare raster packing against u8g2's independent rotation code.
         * Non-byte-aligned width and extra row bytes must ignore padding.
         */
        const uint8_t bitmap[4] = {0xf5, 0xee, 0xea, 0xdd};
        uint8_t reference_buffer[EPD_SSD1685_BUFFER_BYTES];
        u8g2_t reference = d.u8g2;
        u8g2_SetupBuffer(&reference, reference_buffer, EPD_SSD1685_HEIGHT / 8U,
                         u8g2_ll_hvline_vertical_top_lsb, rotations[r]);
        for (unsigned inverted = 0; inverted < 2; ++inverted) {
            const unsigned x = inverted ? u8g2_GetDisplayWidth(&d.u8g2) - 5 : 3;
            const unsigned y = inverted ? u8g2_GetDisplayHeight(&d.u8g2) - 2 : 9;
            memset(reference_buffer, inverted ? 0 : 0xff, sizeof(reference_buffer));
            for (unsigned row = 0; row < 2; ++row)
                for (unsigned column = 0; column < 5; ++column) {
                    bool black = (bitmap[row * 2] & (1U << column)) != 0;
                    u8g2_SetDrawColor(&reference, black == (bool)inverted);
                    u8g2_DrawPixel(&reference, x + column, y + row);
                }
            count = 0;
            assert(epd_ssd1685_present_mono_xbm(&d, bitmap, 4, x, y, 5, 2, 2, inverted) == ESP_OK);
            assert(memcmp(d.buffer, reference_buffer, sizeof(reference_buffer)) == 0);
            assert(memcmp(d.shadow, reference_buffer, sizeof(reference_buffer)) == 0);
            count = 0;
            assert(epd_ssd1685_present_mono_xbm(&d, bitmap, 4, x, y, 5, 2, 2, inverted) == ESP_OK && !count);
        }
        memcpy(reference_buffer, d.buffer, sizeof(reference_buffer));
        assert(epd_ssd1685_present_mono_xbm(&d, bitmap, 3, 0, 0, 5, 2, 2, false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd1685_present_mono_xbm(&d, bitmap, 4, 0, 0, 9, 2, 1, false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd1685_present_mono_xbm(&d, bitmap, 4, 0, 0, 5, 2, 0, false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd1685_present_mono_xbm(&d, bitmap, 4, 0, 0, 0, 2, 2, false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd1685_present_mono_xbm(&d, bitmap, 4, UINT16_MAX, 0, 5, 2, 2, false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd1685_present_mono_xbm(&d, bitmap, 4, 0, UINT16_MAX, 5, 2, 2, false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd1685_present_mono_xbm(&d, NULL, 4, 0, 0, 5, 2, 2, false) == ESP_ERR_INVALID_ARG);
        const u8g2_cb_t unsupported = *U8G2_R0;
        d.u8g2.cb = &unsupported;
        assert(epd_ssd1685_present_mono_xbm(&d, bitmap, 4, 0, 0, 5, 2, 2, false) == ESP_ERR_NOT_SUPPORTED);
        d.u8g2.cb = rotations[r];
        assert(!count && memcmp(d.buffer, reference_buffer, sizeof(reference_buffer)) == 0);
        assert(display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_SET_POWER_SAVE, 1, NULL));
        count = 0;
        assert(epd_ssd1685_present_mono_xbm(&d, bitmap, 4, 0, 0, 5, 2, 2, false) == ESP_ERR_INVALID_STATE);
        assert(!count && memcmp(d.buffer, reference_buffer, sizeof(reference_buffer)) == 0);
        assert(!d.controller_ready && !d.shadow_valid && !d.powered);
        assert(epd_ssd1685_resume(&d) == ESP_OK && d.controller_ready && !d.shadow_valid);
        epd_ssd1685_deinit(&d);
        assert(!d.spi && !d.buffer && !d.shadow && !d.line_buffer);
    }
    c.panel = 0;
    assert(epd_ssd1685_init(&d, &c) == ESP_ERR_INVALID_ARG);
    c.panel = 1; c.dc_pin = c.cs_pin;
    assert(epd_ssd1685_init(&d, &c) == ESP_ERR_INVALID_ARG);
    c.dc_pin = 9; c.spi_clock_hz = 10000001;
    assert(epd_ssd1685_init(&d, &c) == ESP_ERR_INVALID_ARG);
    c.spi_clock_hz = 10000000; bus_error = ESP_FAIL;
    assert(epd_ssd1685_init(&d, &c) == ESP_FAIL);
    assert(!d.spi && !d.buffer && !d.powered);
    bus_error = ESP_OK; count = 0; stuck_busy = true;
    assert(epd_ssd1685_init(&d, &c) == ESP_ERR_TIMEOUT);
    assert(!d.spi && !d.buffer && !d.powered);
    stuck_busy = false;
    count = 0; worker_init_fail = true;
    assert(epd_ssd1685_init(&d, &c) == ESP_ERR_NO_MEM);
    assert(!d.spi && !d.buffer && !d.shadow && !d.scan_worker.task && !d.pixel_budget.pixels);
    worker_init_fail = false;
}

#define RELAX_TYPE epd_ssd1685_t
#define RELAX_REFRESH epd_ssd1685_refresh
#define RELAX_APPLY epd_ssd1685_relax
#define RELAX_MODE epd_ssd1685_set_controller_mode
#define RELAX_PARTIAL EPD_SSD1685_REFRESH_PARTIAL
#define RELAX_COUNT count
#define RELAX_WIRE find(0x24, 0)->data
#include "epd_relax_contract.h"

int main(void)
{
    test_full_and_partial(); test_fastwave_lut(); test_pulse_sync_and_cleanup();
    test_native_cleanup_tuning(); test_pulse_async_retarget_and_recovery();
    test_pulse_busy_failure(); test_pulse_selection_failure();
    test_failures_and_sleep(); test_raster_async_snapshot(); test_init_rotation_and_cleanup();
    test_power_state_replaces_queued_scan();
    test_relax_transaction();
    puts("epd_ssd1685_test: PASS"); return 0;
}
