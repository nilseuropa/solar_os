#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
static SemaphoreHandle_t busy_panel_lock;
static int checked_semaphore_take(SemaphoreHandle_t semaphore, unsigned timeout)
{
    assert(semaphore != busy_panel_lock);
    return xSemaphoreTake(semaphore, timeout);
}
#define xSemaphoreTake checked_semaphore_take

/* Test the real command/refresh implementation with a captured SPI transport. */
#include "../../src/drivers/epd_ssd1677.c"

typedef struct {
    uint8_t command;
    size_t length;
    uint8_t data[SSD1677_BUFFER_BYTES];
} captured_command_t;

static captured_command_t commands[128];
static size_t command_count;
static int dc_level, reset_level = 1;
static int reset_count, power_count;
static bool stuck_busy;
static int fail_after = -1;
static int64_t time_us, busy_until;
static bool missing_busy, worker_init_fail, worker_pending, worker_snapshot_valid;
static uint32_t spi_hz;
esp_err_t gpio_config(const gpio_config_t *config) { assert(config); return ESP_OK; }
esp_err_t gpio_reset_pin(gpio_num_t pin) { (void)pin; return ESP_OK; }
esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *config, int dma)
{ (void)host; (void)config; (void)dma; return ESP_OK; }
esp_err_t spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *config, spi_device_handle_t *device)
{ (void)host; assert(config); *device = (void *)1; return ESP_OK; }
esp_err_t solar_os_bus_spi_add_device(const char *name, const spi_device_interface_config_t *config, spi_device_handle_t *device)
{ assert(strcmp(name, "spi0") == 0); return spi_bus_add_device(SPI2_HOST, config, device); }
esp_err_t spi_bus_remove_device(spi_device_handle_t device) { assert(device); return ESP_OK; }
esp_err_t spi_bus_free(spi_host_device_t host) { (void)host; return ESP_OK; }
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

static uint8_t frame[SSD1677_BUFFER_BYTES], shadow[SSD1677_BUFFER_BYTES];
static uint8_t line[SSD1677_PANEL_ROW_BYTES];
static StaticSemaphore_t panel_lock;

void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0);
    time_us += (int64_t)ticks * SOLAR_OS_EPD_TEST_TICK_MS * 1000;
}

int64_t esp_timer_get_time(void) { return time_us; }
int gpio_get_level(gpio_num_t pin) { (void)pin; return stuck_busy || time_us < busy_until; }

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    if (pin == 9) dc_level = (int)level;
    if (pin == 46) {
        reset_level = (int)level;
        if (level == 0) reset_count++;
    }
    return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *transaction)
{
    assert(device != NULL);
    spi_hz = transaction->override_freq_hz;
    if (fail_after == 0) { fail_after = -1; return ESP_FAIL; }
    if (fail_after > 0) --fail_after;
    const uint8_t *data = transaction->flags & SPI_TRANS_USE_TXDATA ?
        transaction->tx_data : transaction->tx_buffer;
    size_t length = transaction->length / 8U;
    if (dc_level == 0) {
        assert(length == 1 && command_count < 128);
        if (data[0] == 0x20 && !missing_busy) busy_until = time_us + 30000;
        commands[command_count++] = (captured_command_t){.command = data[0]};
    } else {
        assert(command_count > 0);
        captured_command_t *command = &commands[command_count - 1U];
        assert(command->length + length <= sizeof(command->data));
        memcpy(command->data + command->length, data, length);
        command->length += length;
    }
    return ESP_OK;
}

static esp_err_t set_power(void *context, bool on)
{
    (void)context;
    if (!on) return ESP_OK;
    power_count++;
    return ESP_OK;
}

static epd_ssd1677_t make_display(void)
{
    memset(frame, 0xff, sizeof(frame)); /* white */
    memset(shadow, 0, sizeof(shadow));
    command_count = 0;
    return (epd_ssd1677_t) {
        .spi = (void *)1, .buffer = frame, .shadow = shadow, .line_buffer = line,
        .panel = EPD_SSD1677_PANEL_STICKY,
        .lock = &panel_lock,
        .buffer_size = sizeof(frame), .shadow_size = sizeof(shadow),
        .line_buffer_size = sizeof(line), .dc_pin = 9, .reset_pin = 46,
        .busy_pin = 3, .busy_level = 1, .power_pin = -1,
        .set_power = set_power,
        .pulse_tuning = {.drive_cycles=3, .pulse_frames=8, .waveform_fr=8,
            .cleanup_every=64, .cleanup_adaptive=true},
        .scan_period_ms=100, .pulse_spi_clock_hz=20000000, .pulse_power_hold=true,
    };
}

static captured_command_t *find_command(uint8_t command, size_t occurrence)
{
    for (size_t i = 0; i < command_count; i++) {
        if (commands[i].command == command && occurrence-- == 0) return &commands[i];
    }
    assert(!"missing command");
    return NULL;
}

static void expect_data(uint8_t command, const uint8_t *data, size_t length)
{
    captured_command_t *capture = find_command(command, 0);
    assert(capture->length == length && memcmp(capture->data, data, length) == 0);
}

static void test_initial_full(void)
{
    epd_ssd1677_t display = make_display();
    /* Three distinct marks prove conversion order and bottom/top gate mapping. */
    frame[0] &= (uint8_t)~1U;
    frame[799] &= (uint8_t)~1U;
    frame[sizeof(frame) - 1U] &= (uint8_t)~0x80U;
    assert(ssd1677_refresh(&display) == ESP_OK);
    assert(power_count == 1 && reset_count == 1 && reset_level == 1);
    expect_data(0x11, (uint8_t[]){0x01}, 1);
    expect_data(0x44, (uint8_t[]){0, 0, 0x1f, 3}, 4);
    expect_data(0x45, (uint8_t[]){0xdf, 1, 0, 0}, 4);
    expect_data(0x4f, (uint8_t[]){0xdf, 1}, 2);
    expect_data(0x46, (uint8_t[]){0xf7}, 1);
    expect_data(0x47, (uint8_t[]){0xf7}, 1);
    expect_data(0x21, (uint8_t[]){0}, 1);
    expect_data(0x22, (uint8_t[]){0xf7}, 1);
    assert(find_command(0x3c, 1)->data[0] == 0x01);
    assert(!display.analog_on);
    const captured_command_t *current = find_command(0x24, 0);
    const captured_command_t *baseline = find_command(0x26, 0);
    assert(current->length == sizeof(frame) && baseline->length == sizeof(frame));
    assert(current->data[0] == 0x7f && current->data[99] == 0xfe);
    assert(current->data[sizeof(frame) - 1U] == 0xfe);
    for (size_t i = 0; i < sizeof(frame); i++) {
        assert(current->data[i] == baseline->data[i]);
    }
    assert(memcmp(find_command(0x24, 1)->data, current->data, sizeof(frame)) == 0);
    assert(memcmp(find_command(0x26, 1)->data, current->data, sizeof(frame)) == 0);
    assert(display.shadow_valid && memcmp(frame, shadow, sizeof(frame)) == 0);
    command_count = 0;
    assert(ssd1677_refresh(&display) == ESP_OK && command_count == 0);
}

static void test_partial_and_cleanup(void)
{
    epd_ssd1677_t display = make_display();
    display.controller_ready = true;
    display.shadow_valid = true;
    memcpy(shadow, frame, sizeof(frame));
    /* Dirty byte at x=16..23, logical y=10: controller gate Y=469. */
    frame[800 + 17] &= (uint8_t)~4U;
    const int resets_before = reset_count;
    assert(ssd1677_refresh(&display) == ESP_OK);
    assert(reset_count == resets_before);
    expect_data(0x44, (uint8_t[]){16, 0, 23, 0}, 4);
    expect_data(0x45, (uint8_t[]){0xd5, 1, 0xd5, 1}, 4);
    expect_data(0x4f, (uint8_t[]){0xd5, 1}, 2);
    expect_data(0x24, (uint8_t[]){0xbf}, 1);
    expect_data(0x26, (uint8_t[]){0xff}, 1);
    assert(find_command(0x26, 1)->data[0] == 0xbf);
    assert(display.partial_refresh_count == 1);
    expect_data(0x22, (uint8_t[]){0xfc}, 1);
    assert(display.analog_on);

    /* Periodic cleanup must force a full transition without resetting RAM. */
    display.partial_refresh_count = SSD1677_AUTO_FULL_INTERVAL - 1U;
    frame[0] &= (uint8_t)~1U;
    command_count = 0;
    assert(ssd1677_refresh(&display) == ESP_OK);
    assert(find_command(0x24, 0)->length == sizeof(frame));
    assert(find_command(0x26, 0)->data[0] == 0x7f);
    expect_data(0x22, (uint8_t[]){0xf7}, 1);
    assert(display.partial_refresh_count == 0 && reset_count == resets_before);
}

static void test_generic_preserves_native_sequence(void)
{
    epd_ssd1677_t d = make_display();
    d.panel = EPD_SSD1677_PANEL_GENERIC;
    assert(ssd1677_refresh(&d) == ESP_OK);
    expect_data(0x22, (uint8_t[]){0xfc}, 1);
    assert(find_command(0x26, 0)->data[0] == 0);
    assert(d.analog_on);
}

static void test_failure_and_no_shadow(void)
{
    epd_ssd1677_t display = make_display();
    display.controller_ready = true;
    display.shadow_valid = true;
    memcpy(shadow, frame, sizeof(frame));
    frame[0] &= (uint8_t)~1U;
    stuck_busy = true;
    assert(ssd1677_refresh(&display) == ESP_ERR_TIMEOUT);
    assert(!display.shadow_valid && !display.controller_ready);
    assert(shadow[0] == 0xff);
    stuck_busy = false;

    display = make_display();
    display.controller_ready = true;
    display.shadow = NULL;
    display.shadow_valid = true;
    assert(ssd1677_refresh(&display) == ESP_OK);
    assert(find_command(0x24, 0)->length == sizeof(frame));
    assert(find_command(0x26, 0)->data[0] == 0xff);
}


static void test_fastwave_layout(void)
{
    uint8_t lut[SOLAR_OS_SSD1677_LUT_BYTES];
    assert(!solar_os_ssd1677_waveform_build(lut, 0, 8, false));
    assert(!solar_os_ssd1677_waveform_build(lut, 8, 9, false));
    for (unsigned fr = 1; fr <= 8; ++fr) {
        for (unsigned frames = 1; frames <= 32; ++frames) {
            for (unsigned invert = 0; invert < 2; ++invert) {
                assert(solar_os_ssd1677_waveform_build(lut, frames, fr, invert));
                for (unsigned i = 0; i < sizeof(lut); ++i) {
                    uint8_t want = i == 10 ? (invert ? 0x40 : 0x80) :
                        i == 20 ? (invert ? 0x80 : 0x40) :
                        i == 50 ? frames : i == 51 ? 1 : i >= 100 ? fr * 17 : 0;
                    assert(lut[i] == want);
                }
            }
        }
    }
}

static void pulse_display(epd_ssd1677_t *d, bool async)
{
    *d = make_display();
    d->scan_async = async;
    assert(epd_ssd1677_set_controller_mode(d, "waveform=fastwave") == ESP_OK);
    assert(ssd1677_refresh(d) == ESP_OK);
    if (async) assert(worker_tick(&d->scan_worker) == ESP_OK);
    command_count = 0;
}

static void dispose_pulse(epd_ssd1677_t *d)
{
    scan_disable(d);
    epd_scan_worker_deinit(&d->scan_worker);
    free_budget(d);
}

static void test_pulse_masks_and_settling(void)
{
    epd_ssd1677_t d;
    pulse_display(&d, false);
    assert(!d.pixel_budget.pending && d.shadow_valid);
    frame[0] &= (uint8_t)~1U;
    frame[799] &= (uint8_t)~1U;
    assert(ssd1677_refresh(&d) == ESP_OK);
    assert(!d.pixel_budget.pending && d.pixel_budget.peak_drive_cycles == 3);
    expect_data(0x22, (uint8_t[]){0x80}, 1);
    assert(find_command(0x32, 0)->length == 105);
    assert(find_command(0x32, 0)->data[54] == 0);
    assert(find_command(0x26, 0)->data[0] == 0x80);
    assert(find_command(0x26, 0)->data[99] == 0x01);
    assert(find_command(0x24, 0)->data[0] == 0);
    assert(d.pulse_updates == 1 && !d.analog_on && spi_hz == 20000000);
    command_count = 0;
    assert(ssd1677_refresh(&d) == ESP_OK && !command_count);
    frame[0] |= 1;
    command_count = 0;
    assert(ssd1677_refresh(&d) == ESP_OK);
    assert(find_command(0x24, 0)->data[0] == 0x80);
    assert(find_command(0x26, 0)->data[0] == 0);
    assert(find_command(0x26, 0)->data[99] == 0); /* settled black is neutral */
    assert(d.pixel_budget.peak_drive_cycles == 6);
    command_count = 0;
    d.pulse_updates = d.pulse_tuning.cleanup_every * 4;
    assert(ssd1677_refresh(&d) == ESP_OK);
    assert(d.pulse_updates == 0 && d.pixel_budget.peak_drive_cycles == 0);
    assert(find_command(0x24, 0)->length == sizeof(frame));
    assert(!d.fastwave_loaded);
    dispose_pulse(&d);
}

static void test_async_retarget_failure_and_power(void)
{
    epd_ssd1677_t d;
    pulse_display(&d, true);
    frame[0] &= (uint8_t)~1U;
    assert(ssd1677_refresh(&d) == ESP_OK && !command_count);
    assert(worker_tick(&d.scan_worker) == ESP_OK && worker_pending);
    assert(d.pixel_budget.pending == 1 && d.pixel_budget.peak_drive_cycles == 1);
    command_count = 0;
    assert(ssd1677_refresh(&d) == ESP_OK); /* identical input must not refill */
    assert(worker_tick(&d.scan_worker) == ESP_OK && worker_pending);
    assert(d.pulse_updates == 1);
    command_count = 0;
    frame[0] |= 1;
    assert(ssd1677_refresh(&d) == ESP_OK);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    assert(find_command(0x24, 0)->data[0] == 0x80);
    command_count = 0;
    assert(epd_ssd1677_present_power_state(&d) == ESP_OK);
    assert(!d.scan_worker.enabled && d.power_state_present && !d.fastwave_loaded);
    assert(memcmp(frame, shadow, sizeof(frame)) == 0);
    dispose_pulse(&d);

    pulse_display(&d, true);
    frame[0] &= (uint8_t)~1U;
    assert(ssd1677_refresh(&d) == ESP_OK);
    missing_busy = true;
    assert(worker_tick(&d.scan_worker) == ESP_ERR_TIMEOUT);
    missing_busy = false;
    assert(d.refresh_mode == EPD_SSD1677_REFRESH_AUTO && !d.scan_worker.enabled);
    assert(d.shadow_valid && shadow[0] == frame[0]); /* native recovery */
    assert(d.pixel_budget.pending == 1); /* failed waveform consumed no budget */
    assert(epd_ssd1677_set_controller_mode(&d, "waveform=otp") == ESP_OK);
    assert(!d.pixel_budget.pixels && !d.scan_worker.task);
    dispose_pulse(&d);
}

static void test_mode_limits_and_rollback(void)
{
    epd_ssd1677_t d = make_display();
    d.scan_async = true;
    worker_init_fail = true;
    assert(epd_ssd1677_set_controller_mode(&d, "refresh=pulse") == ESP_ERR_NO_MEM);
    assert(d.refresh_mode == EPD_SSD1677_REFRESH_AUTO && !d.pixel_budget.pixels);
    worker_init_fail = false;
    const char *invalid[] = {"waveform-fr=9", "waveform-fr=0", "pulse-period=66",
        "pulse-spi-khz=20001", "pulse-frames=33", "pulse-period=100ms"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i)
        assert(epd_ssd1677_set_controller_mode(&d, invalid[i]) == ESP_ERR_INVALID_ARG);
    assert(d.pulse_tuning.waveform_fr == 8 && d.scan_period_ms == 100);
    assert(epd_ssd1677_set_controller_mode(&d, "refresh=fast") == ESP_OK);
    assert(d.refresh_mode == EPD_SSD1677_REFRESH_PARTIAL);
    assert(epd_ssd1677_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(strstr(epd_ssd1677_controller_mode(&d), "waveform=fastwave"));
    assert(epd_ssd1677_set_controller_mode(&d, "waveform=otp") == ESP_OK);
    assert(!d.pixel_budget.pixels && !d.scan_worker.enabled);
    assert(!d.scan_worker.task && !d.scan_worker.latest && !d.scan_worker.snapshot);
    assert(epd_ssd1677_set_controller_mode(&d, "waveform=fastwave") == ESP_OK);
    assert(d.scan_worker.task && d.scan_worker.latest && d.scan_worker.snapshot);
    assert(epd_ssd1677_set_controller_mode(&d, "pulse-scan=sync") == ESP_OK);
    assert(!d.scan_worker.task && !d.scan_worker.latest && !d.scan_worker.snapshot);
    assert(d.pixel_budget.pixels);
    assert(epd_ssd1677_set_controller_mode(&d, "pulse-scan=async") == ESP_OK);
    assert(d.scan_worker.task && d.scan_worker.latest && d.scan_worker.snapshot);
    dispose_pulse(&d);
}

static void test_pulse_relax_and_sleep(void)
{
    epd_ssd1677_t d;
    pulse_display(&d, true);
    frame[0] &= (uint8_t)~1U;
    assert(ssd1677_refresh(&d) == ESP_OK);
    assert(worker_tick(&d.scan_worker) == ESP_OK && worker_pending);
    command_count = 0;
    assert(epd_ssd1677_relax(&d, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    assert(!d.scan_worker.enabled && !d.fastwave_loaded);
    command_count = 0;
    assert(epd_ssd1677_relax(&d, SOLAR_OS_DISPLAY_RELAX_WHITE) == ESP_OK);
    command_count = 0;
    assert(epd_ssd1677_relax(&d, SOLAR_OS_DISPLAY_RELAX_END) == ESP_OK);
    assert(d.refresh_mode == EPD_SSD1677_REFRESH_PULSE && d.scan_worker.enabled);
    assert(!d.pixel_budget.pending && !d.pulse_needs_cleanup && shadow[0] == frame[0]);
    command_count = 0;
    assert(ssd1677_sleep(&d) == ESP_OK);
    assert(d.suspended && !d.powered && !d.scan_worker.enabled);
    assert(ssd1677_refresh(&d) == ESP_ERR_INVALID_STATE);
    command_count = 0;
    assert(ssd1677_u8x8_display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_SET_POWER_SAVE, 0, NULL));
    assert(!d.suspended && d.powered && d.scan_worker.enabled);
    command_count = 0;
    assert(ssd1677_refresh(&d) == ESP_OK);
    assert(worker_tick(&d.scan_worker) == ESP_OK);
    assert(d.shadow_valid && !d.pulse_needs_cleanup);
    dispose_pulse(&d);
}

#define RELAX_TYPE epd_ssd1677_t
#define RELAX_REFRESH ssd1677_refresh
#define RELAX_APPLY epd_ssd1677_relax
#define RELAX_MODE epd_ssd1677_set_controller_mode
#define RELAX_PARTIAL EPD_SSD1677_REFRESH_PARTIAL
#define RELAX_COUNT command_count
#define RELAX_WIRE find_command(0x24, 0)->data
#include "epd_relax_contract.h"

static void test_submit_during_scan(void)
{
    epd_ssd1677_t d = make_display();
    pulse_display(&d, true);
    const size_t previous_commands = command_count;
    const uint32_t previous_submitted = d.scan_worker.stats.submitted;
    frame[0] ^= 1U;
    busy_panel_lock = d.lock;
    for (unsigned row = 0; row < SSD1677_HEIGHT / 8U; ++row) {
        u8x8_tile_t tile = {.x_pos = 0, .y_pos = row,
            .cnt = SSD1677_WIDTH / 8U,
            .tile_ptr = frame + row * SSD1677_WIDTH};
        assert(ssd1677_u8x8_display_cb(&d.u8g2.u8x8,
            U8X8_MSG_DISPLAY_DRAW_TILE, 1, &tile) == 1);
    }
    assert(ssd1677_u8x8_display_cb(&d.u8g2.u8x8,
        U8X8_MSG_DISPLAY_REFRESH, 0, NULL) == 1);
    busy_panel_lock = NULL;
    assert(command_count == previous_commands);
    assert(d.scan_worker.stats.submitted == previous_submitted + 1U);
    assert(memcmp(d.scan_worker.latest, frame, sizeof(frame)) == 0);
    assert(worker_tick(&d.scan_worker) == ESP_OK && worker_pending);
    dispose_pulse(&d);
}

static void test_pulse_window_clears_previous_mask(void)
{
    epd_ssd1677_t d;
    pulse_display(&d, false);
    frame[800 + 17] &= (uint8_t)~4U;
    assert(ssd1677_refresh(&d) == ESP_OK);
    assert(find_command(0x26, 0)->length == sizeof(frame));
    command_count = 0;
    frame[800 + 33] &= (uint8_t)~4U;
    assert(ssd1677_refresh(&d) == ESP_OK);
    expect_data(0x44, (uint8_t[]){16, 0, 39, 0}, 4);
    expect_data(0x45, (uint8_t[]){0xd5, 1, 0xd5, 1}, 4);
    expect_data(0x26, (uint8_t[]){0, 0, 0x40}, 3);
    expect_data(0x24, (uint8_t[]){0, 0, 0}, 3);
    command_count = 0;
    frame[800 + 33] |= 4;
    assert(ssd1677_refresh(&d) == ESP_OK);
    expect_data(0x44, (uint8_t[]){32, 0, 39, 0}, 4);
    expect_data(0x24, (uint8_t[]){0x40}, 1);
    expect_data(0x26, (uint8_t[]){0}, 1);
    dispose_pulse(&d);
}

static void test_pulse_byte_diff_coordinates(void)
{
    epd_ssd1677_t d;
    pulse_display(&d, true);
    for (size_t i = 0; i < sizeof(frame); i++) frame[i] = (uint8_t)(i * 73U);
    bool baseline = true;
    assert(pulse_prepare(&d, frame, true, &baseline) == ESP_OK && !baseline);
    size_t pending = 0;
    for (size_t p = 0; p < SSD1677_WIDTH * SSD1677_HEIGHT; p++) {
        const size_t x = p % SSD1677_WIDTH, y = p / SSD1677_WIDTH;
        const bool white = (frame[y / 8U * SSD1677_WIDTH + x] & (1U << (y % 8U))) != 0;
        assert(epd_pixel_budget_drive(&d.pixel_budget, p) ==
            (white ? EPD_PIXEL_HOLD : EPD_PIXEL_DRIVE_BLACK));
        if (!white) pending++;
    }
    assert(d.pixel_budget.pending == pending);
    assert(memcmp(d.shadow, frame, sizeof(frame)) == 0);
    dispose_pulse(&d);
}

static void test_startup_profile(void)
{
    for (unsigned panel = EPD_SSD1677_PANEL_GENERIC; panel <= EPD_SSD1677_PANEL_STICKY; panel++) {
        for (unsigned fail_worker = 0; fail_worker <= 1; fail_worker++) {
            epd_ssd1677_t d;
            epd_ssd1677_config_t config = {
                .spi_bus = "spi0", .spi_host = SPI2_HOST,
                .cs_pin = 10, .dc_pin = 9, .reset_pin = 46,
                .busy_pin = 3, .power_pin = -1, .busy_level = 1,
                .spi_clock_hz = panel == EPD_SSD1677_PANEL_STICKY ? 20000000 : 10000000,
                .panel = panel, .rotation = U8G2_R0,
            };
            command_count = 0;
            worker_init_fail = fail_worker;
            assert(epd_ssd1677_init(&d, &config) == ESP_OK);
            worker_init_fail = false;
            const bool pulse = panel == EPD_SSD1677_PANEL_STICKY && !fail_worker;
            assert(d.refresh_mode == (pulse ? EPD_SSD1677_REFRESH_PULSE : EPD_SSD1677_REFRESH_AUTO));
            assert((d.scan_worker.task != NULL) == pulse);
            assert((d.pixel_budget.pixels != NULL) == pulse);
            assert(d.pulse_tuning.drive_cycles == (panel == EPD_SSD1677_PANEL_STICKY ? 5 : 3));
            if (pulse) {
                assert(d.scan_async && d.scan_worker.enabled && d.scan_submit_ready);
                assert(d.pulse_spi_clock_hz == 20000000);
                assert(d.pulse_tuning.pulse_frames == 8);
                assert(d.pulse_tuning.waveform_fr == 8 && d.scan_period_ms == 100);
                assert(ssd1677_refresh(&d) == ESP_OK);
                assert(worker_tick(&d.scan_worker) == ESP_OK && d.shadow_valid);
                assert(spi_hz == 20000000);
            }
            epd_ssd1677_deinit(&d);
        }
    }
}

static void test_mono_raster(void)
{
    const u8g2_cb_t *rotations[] = {U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3};
    const uint8_t bitmap[] = {0xf5, 0xee, 0xea, 0xdd};
    uint8_t expected[SSD1677_BUFFER_BYTES];
    for (unsigned r = 0; r < 4; r++) {
        epd_ssd1677_t d;
        epd_ssd1677_config_t config = {
            .spi_bus = "spi0", .spi_host = SPI2_HOST,
            .cs_pin = 10, .dc_pin = 9, .reset_pin = 46,
            .busy_pin = 3, .power_pin = -1, .busy_level = 1,
            .spi_clock_hz = 20000000, .panel = EPD_SSD1677_PANEL_STICKY,
            .rotation = rotations[r],
        };
        command_count = 0;
        assert(epd_ssd1677_init(&d, &config) == ESP_OK);
        u8g2_t reference = d.u8g2;
        u8g2_SetupBuffer(&reference, expected, SSD1677_TILE_HEIGHT,
            u8g2_ll_hvline_vertical_top_lsb, rotations[r]);
        for (unsigned inverted = 0; inverted < 2; inverted++) {
            const unsigned x = inverted ? u8g2_GetDisplayWidth(&d.u8g2) - 5U : 3U;
            const unsigned y = inverted ? u8g2_GetDisplayHeight(&d.u8g2) - 2U : 9U;
            memset(expected, inverted ? 0 : 0xff, sizeof(expected));
            for (unsigned row = 0; row < 2; row++) {
                for (unsigned column = 0; column < 5; column++) {
                    const bool black = (bitmap[row * 2U] & (1U << column)) != 0;
                    u8g2_SetDrawColor(&reference, black == (bool)inverted);
                    u8g2_DrawPixel(&reference, x + column, y + row);
                }
            }
            command_count = 0;
            busy_panel_lock = d.lock;
            assert(epd_ssd1677_present_mono_xbm(&d, bitmap, sizeof(bitmap),
                x, y, 5, 2, 2, inverted) == ESP_OK);
            busy_panel_lock = NULL;
            assert(!command_count && memcmp(d.buffer, expected, sizeof(expected)) == 0);
            assert(memcmp(d.scan_worker.latest, expected, sizeof(expected)) == 0);
            assert(worker_tick(&d.scan_worker) == ESP_OK);
            assert(memcmp(d.shadow, expected, sizeof(expected)) == 0);
        }
        command_count = 0;
        assert(epd_ssd1677_present_mono_xbm(&d, bitmap, 3, 0, 0, 5, 2, 2, false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd1677_present_mono_xbm(&d, bitmap, 4, 0, 0, 9, 2, 1, false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd1677_present_mono_xbm(&d, bitmap, 4, 0, 0, 5, 2, 0, false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd1677_present_mono_xbm(&d, bitmap, 4, 0, 0, 0, 2, 2, false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd1677_present_mono_xbm(&d, bitmap, 4, 0, 0, 5, 0, 2, false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd1677_present_mono_xbm(&d, bitmap, 4, UINT16_MAX, 0, 5, 2, 2, false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd1677_present_mono_xbm(&d, bitmap, 4, 0, UINT16_MAX, 5, 2, 2, false) == ESP_ERR_INVALID_ARG);
        assert(epd_ssd1677_present_mono_xbm(&d, NULL, 4, 0, 0, 5, 2, 2, false) == ESP_ERR_INVALID_ARG);
        const u8g2_cb_t unsupported = *U8G2_R0;
        d.u8g2.cb = &unsupported;
        assert(epd_ssd1677_present_mono_xbm(&d, bitmap, 4, 0, 0, 5, 2, 2, false) == ESP_ERR_NOT_SUPPORTED);
        d.u8g2.cb = rotations[r];
        d.power_state_present = true;
        assert(epd_ssd1677_present_mono_xbm(&d, bitmap, 4, 0, 0, 5, 2, 2, false) == ESP_ERR_INVALID_STATE);
        d.power_state_present = false;
        d.suspended = true;
        assert(epd_ssd1677_present_mono_xbm(&d, bitmap, 4, 0, 0, 5, 2, 2, false) == ESP_ERR_INVALID_STATE);
        d.suspended = false;
        assert(!command_count && memcmp(d.buffer, expected, sizeof(expected)) == 0);
        epd_ssd1677_deinit(&d);
    }
}

int main(void)
{
    test_initial_full();
    test_partial_and_cleanup();
    test_generic_preserves_native_sequence();
    test_failure_and_no_shadow();
    test_relax_transaction();
    test_fastwave_layout();
    test_pulse_masks_and_settling();
    test_async_retarget_failure_and_power();
    test_mode_limits_and_rollback();
    test_pulse_relax_and_sleep();
    test_submit_during_scan();
    test_pulse_window_clears_previous_mask();
    test_pulse_byte_diff_coordinates();
    test_startup_profile();
    test_mono_raster();
    puts("epd_ssd1677_test: PASS");
    return 0;
}
