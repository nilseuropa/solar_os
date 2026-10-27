#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../../src/apps/solar_os_rfscan.c"

struct solar_os_radio_scan_worker { solar_os_radio_scan_t view; };
static struct solar_os_radio_scan_worker worker;
static unsigned starts, stops;
static char buttons[5][16];
static unsigned button_count;
static char frequency_line[96];
static char threshold_line[96];
static char header_line[96];
static bool busy;
static bool fail_next_start;
static unsigned status_lines;
static int64_t now_us = 1000000;
static unsigned beeps, cancellations;
static uint32_t last_beep;
static esp_err_t audio_error;
static const uint32_t original_hz = 869618000U;

size_t strlcpy(char *out, const char *in, size_t size)
{
    size_t n = strlen(in);
    if (size) { size_t copy = n < size - 1 ? n : size - 1; memcpy(out, in, copy); out[copy] = 0; }
    return n;
}
int64_t esp_timer_get_time(void) { return now_us; }
esp_err_t solar_os_audio_tone_enqueue(const solar_os_audio_tone_request_t *request, uint32_t *id)
{
    assert(request->step_count == 1 && request->steps[0].frequency_hz == 1200);
    assert(request->steps[0].duration_ms == 70 && request->drop_if_busy);
    assert(request->volume == SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    if (audio_error != ESP_OK) { *id = 0; return audio_error; }
    *id = ++beeps; last_beep = *id; return ESP_OK;
}
esp_err_t solar_os_audio_tone_cancel(uint32_t id)
{ assert(id == last_beep); cancellations++; return ESP_OK; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "error"; }
solar_os_gfx_t *solar_os_context_gfx(solar_os_context_t *ctx) { return ctx->gfx; }
void solar_os_context_set_graphics_active(solar_os_context_t *ctx, bool active) { (void)ctx; (void)active; }
int solar_os_context_argc(const solar_os_context_t *ctx) { (void)ctx; return 2; }
const char *solar_os_context_argv(const solar_os_context_t *ctx, int index)
{ (void)ctx; return index == 0 ? "rfscan" : index == 1 ? "radio0" : NULL; }
void solar_os_context_finish(solar_os_context_t *ctx, int code, const char *message)
{ (void)code; (void)message; ctx->exit_requested = true; }

size_t solar_os_gfx_width(const solar_os_gfx_t *gfx) { (void)gfx; return 320; }
size_t solar_os_gfx_height(const solar_os_gfx_t *gfx) { (void)gfx; return 240; }
solar_os_gfx_color_t solar_os_gfx_gray(uint8_t level) { return level; }
void solar_os_gfx_set_color(solar_os_gfx_t *g, solar_os_gfx_color_t c) { (void)g; (void)c; }
void solar_os_gfx_set_font(solar_os_gfx_t *g, solar_os_gfx_font_t f) { (void)g; (void)f; }
void solar_os_gfx_set_line_style(solar_os_gfx_t *g, solar_os_gfx_line_style_t s) { (void)g; (void)s; }
size_t solar_os_gfx_text_width(solar_os_gfx_t *g, const char *s) { (void)g; return strlen(s) * 6; }
void solar_os_gfx_clear(solar_os_gfx_t *g, solar_os_gfx_color_t c)
{ (void)g; (void)c; button_count = 0; status_lines = 0; }
void solar_os_gfx_pixel(solar_os_gfx_t *g, int x, int y) { (void)g; (void)x; (void)y; }
void solar_os_gfx_line(solar_os_gfx_t *g, int a, int b, int c, int d) { (void)g; (void)a; (void)b; (void)c; (void)d; }
void solar_os_gfx_rect(solar_os_gfx_t *g, int a, int b, int c, int d) { (void)g; (void)a; (void)b; (void)c; (void)d; }
void solar_os_gfx_fill_rect(solar_os_gfx_t *g, int a, int b, int c, int d) { (void)g; (void)a; (void)b; (void)c; (void)d; }
void solar_os_gfx_present(solar_os_gfx_t *g) { (void)g; }
void solar_os_gfx_text(solar_os_gfx_t *g, int x, int y, const char *text)
{
    (void)g; (void)x;
    if (y == 12) strlcpy(header_line, text, sizeof(header_line));
    assert(strcmp(text, "Channel energy") && !strstr(text, "sweeps"));
    assert(y != 210); /* No text footer above the bottom controls. */
    if (y == 52 || y == 64 || y == 76) {
        status_lines++;
        if (strlen(text) * 6 > 312) fprintf(stderr, "Status line too wide: %s\n", text);
        assert(strlen(text) * 6 <= 312);
    }
    if (y == 52) strlcpy(frequency_line, text, sizeof(frequency_line));
    if (y == 76) strlcpy(threshold_line, text, sizeof(threshold_line));
    if (y == 230) { assert(button_count < 5); strlcpy(buttons[button_count++], text, sizeof(buttons[0])); }
}

size_t solar_os_radio_count(void) { return 1; }
esp_err_t solar_os_radio_get_info(const char *name, solar_os_radio_info_t *out)
{
    assert(!strcmp(name, "radio0"));
    memset(out, 0, sizeof(*out)); strlcpy(out->name, "radio0", sizeof(out->name));
    out->claimed = busy; if (busy) strlcpy(out->owner, "meshcore:test", sizeof(out->owner));
    return ESP_OK;
}
bool solar_os_radio_get(size_t index, solar_os_radio_info_t *out)
{ return index == 0 && solar_os_radio_get_info("radio0", out) == ESP_OK; }
esp_err_t solar_os_radio_get_status(const char *name, solar_os_radio_status_t *out)
{
    (void)name; memset(out, 0, sizeof(*out));
    out->config.frequency_hz = original_hz; out->config.rx_bandwidth_hz = 62500;
    out->state = SOLAR_OS_RADIO_STATE_RX; return ESP_OK;
}
void solar_os_radio_scan_clear(solar_os_radio_scan_t *scan)
{
    scan->history_count = scan->bins = scan->rows = 0;
    scan->latest = scan->floor = scan->peak = SOLAR_OS_RADIO_SCAN_EMPTY; scan->generation++;
    scan->recent_peak = SOLAR_OS_RADIO_SCAN_EMPTY;
    scan->recent_peak_frequency_hz = 0;
}
void solar_os_radio_scan_init(solar_os_radio_scan_t *scan)
{ memset(scan, 0, sizeof(*scan)); solar_os_radio_scan_clear(scan); }
esp_err_t solar_os_radio_scan_worker_open(solar_os_radio_scan_worker_t **out,
    const char *name, const solar_os_radio_scan_config_t *config)
{
    assert(*out == NULL); starts++;
    if (fail_next_start) { fail_next_start = false; return ESP_ERR_INVALID_ARG; }
    if (busy && config->frequency_hz) return ESP_ERR_INVALID_STATE;
    solar_os_radio_scan_init(&worker.view); worker.view.config = *config;
    solar_os_radio_status_t status; solar_os_radio_get_status(name, &status);
    worker.view.receiver = status.config;
    worker.view.restore = config->mode == SOLAR_OS_RADIO_SCAN_BAND || config->frequency_hz != 0;
    if (config->frequency_hz) worker.view.receiver.frequency_hz = config->frequency_hz;
    worker.view.running = true; *out = &worker; return ESP_OK;
}
esp_err_t solar_os_radio_scan_worker_copy(solar_os_radio_scan_worker_t *w, solar_os_radio_scan_t *out)
{
    *out = w->view; w->view.recent_peak = SOLAR_OS_RADIO_SCAN_EMPTY;
    w->view.recent_peak_frequency_hz = 0; return ESP_OK;
}
esp_err_t solar_os_radio_scan_worker_close(solar_os_radio_scan_worker_t **w, solar_os_radio_scan_t *out)
{
    if (*w) { stops++; if (out) { *out = (*w)->view; out->running = false; } *w = NULL; }
    return ESP_OK;
}
bool solar_os_radio_scan_worker_done(const solar_os_radio_scan_worker_t *w) { (void)w; return true; }
esp_err_t solar_os_radio_scan_worker_clear(solar_os_radio_scan_worker_t *w)
{ solar_os_radio_scan_clear(&w->view); return ESP_OK; }

static void key(solar_os_context_t *ctx, uint8_t ch)
{
    solar_os_event_t event = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = (char)ch};
    assert(rfscan_event(ctx, &event));
    assert(button_count == 5);
    assert(status_lines == 3);
}

static void input(solar_os_context_t *ctx, const char *text)
{ while (*text) key(ctx, (uint8_t)*text++); }

static void tick(solar_os_context_t *ctx, unsigned milliseconds)
{
    now_us += (int64_t)milliseconds * 1000;
    if (rfscan.worker != NULL && worker.view.recent_peak != SOLAR_OS_RADIO_SCAN_EMPTY)
        worker.view.generation++;
    solar_os_event_t event = {.type = SOLAR_OS_EVENT_TICK};
    (void)rfscan_event(ctx, &event);
}

static void test_y_scale(void)
{
    void *saved = rfscan_state;
    rfscan_state = calloc(1, sizeof(rfscan_state_t)); assert(rfscan_state);
    solar_os_radio_scan_init(&rfscan.scan);
    rfscan.average_dbm = -80; rfscan.y_zoom = 4; rfscan.window_ms = 200;
    rfscan.scan.history_count = rfscan.scan.history_head = 4;
    const int16_t values[] = {-20, -110, -100, -90};
    const uint32_t times[] = {0, 800, 900, 1000};
    memcpy(rfscan.scan.history, values, sizeof(values));
    memcpy(rfscan.scan.history_ms, times, sizeof(times));
    rfscan.scan.latest = -90; rfscan.scan.peak = -5;
    rfscan_center_y(); assert(rfscan.average_dbm == -100); /* Excludes old samples and held peaks. */
    assert(rfscan_y(-100, 100, 101) == 150);
    assert(rfscan_y(-50, 100, 101) == 100 && rfscan_y(-150, 100, 101) == 200);
    assert(rfscan_y(-20, 100, 101) == 100 && rfscan_y(-200, 100, 101) == 200);
    rfscan.scan.history_count = 3; rfscan.scan.history_head = 2;
    rfscan.scan.history[255] = -110; rfscan.scan.history_ms[255] = 800;
    rfscan.scan.history[0] = -100; rfscan.scan.history_ms[0] = 900;
    rfscan.scan.history[1] = -90; rfscan.scan.history_ms[1] = 1000;
    rfscan_center_y(); assert(rfscan.average_dbm == -100); /* Wrapped history. */
    rfscan.band = true; rfscan.scan.bins = 4;
    rfscan.scan.levels[0] = -110; rfscan.scan.levels[1] = SOLAR_OS_RADIO_SCAN_EMPTY;
    rfscan.scan.levels[2] = -90; rfscan.scan.levels[3] = SOLAR_OS_RADIO_SCAN_EMPTY;
    rfscan_center_y(); assert(rfscan.average_dbm == -100);
    rfscan.y_fixed = true;
    rfscan.scan.levels[0] = -80; rfscan.scan.levels[2] = -60;
    rfscan_center_y(); assert(rfscan.average_dbm == -70 && rfscan.y_center_dbm == -100);
    assert(rfscan_y(-100, 100, 101) == 150); /* Live average cannot move a locked axis. */
    rfscan_zoom(-1);
    assert(rfscan.y_center_dbm == -70 && rfscan_y(-70, 100, 101) == 150);
    rfscan.scan.levels[0] = -110; rfscan.scan.levels[2] = -90;
    rfscan_center_y(); assert(rfscan.average_dbm == -100 && rfscan.y_center_dbm == -70);
    rfscan_zoom(1); assert(rfscan.y_center_dbm == -100); /* Recenter only on zoom. */
    const unsigned increments[] = {1, 1, 1, 2, 5, 10};
    for (unsigned i = 0; i < 6; i++) {
        rfscan.y_zoom = i; assert(rfscan_threshold_step() == increments[i]);
    }
    rfscan.y_zoom = 0;
    assert(rfscan_y(-100, 88, 51) == 113); /* Odd 5 dB span centers exactly. */
    assert(rfscan_y_visible(-100) && !rfscan_y_visible(-90));
    solar_os_radio_scan_clear(&rfscan.scan);
    rfscan_center_y(); assert(rfscan.average_dbm == -100); /* Empty graph keeps its center. */
    free(rfscan_state); rfscan_state = saved;
}

int main(void)
{
    rfscan_state = calloc(1, sizeof(rfscan_state_t)); assert(rfscan_state);
    solar_os_context_t ctx = {.gfx = (solar_os_gfx_t *)&ctx};
    assert(rfscan_start(&ctx) == ESP_OK && rfscan.scan.running);
    test_y_scale();
    assert(!strcmp(buttons[2], "Step") && !strcmp(buttons[3], "Freq"));
    assert(!strcmp(buttons[4], "Thresh"));
    assert(worker.view.config.frequency_hz == 0 && worker.view.config.bandwidth_hz == 0);
    assert(strstr(header_line, "LIVE observe RX"));
    assert(strstr(header_line, "Time: 10s"));
    assert(!strcmp(frequency_line, "Freq: 869.618000MHz BW: 62.500kHz Step: 125kHz"));
    assert(!strcmp(threshold_line, "Threshold: >=-90dBm Triggered: --"));
    key(&ctx, SOLAR_OS_KEY_PAGE_UP); assert(rfscan_y_spans[rfscan.y_zoom] == 50);
    key(&ctx, SOLAR_OS_KEY_PAGE_DOWN); assert(rfscan_y_spans[rfscan.y_zoom] == 100);
    for (unsigned i = 0; i < 10; i++) key(&ctx, SOLAR_OS_KEY_PAGE_UP);
    assert(rfscan_y_spans[rfscan.y_zoom] == 5);
    for (unsigned i = 0; i < 10; i++) key(&ctx, SOLAR_OS_KEY_PAGE_DOWN);
    assert(rfscan_y_spans[rfscan.y_zoom] == 200);
    key(&ctx, SOLAR_OS_KEY_PAGE_UP);
    assert(starts == 1 && stops == 0 && rfscan.scan.running && rfscan.threshold_dbm == -90);
    key(&ctx, SOLAR_OS_KEY_RIGHT); key(&ctx, SOLAR_OS_KEY_RIGHT);
    assert(rfscan.focus == 2);
    key(&ctx, SOLAR_OS_KEY_UP);
    assert(rfscan.step_hz == 250000 && rfscan.scan.running && starts == 1 && stops == 0);
    key(&ctx, SOLAR_OS_KEY_RIGHT); key(&ctx, SOLAR_OS_KEY_UP);
    assert(rfscan.center_hz == original_hz + 250000U && rfscan.scan.running);
    assert(strstr(frequency_line, "869.868000"));
    assert(worker.view.config.frequency_hz == original_hz + 250000U);
    assert(worker.view.config.bandwidth_hz == 0 && rfscan.scan.running);
    assert(strstr(header_line, "LIVE owns RX"));
    rfscan.focus = 3; key(&ctx, SOLAR_OS_KEY_DOWN);
    assert(rfscan.center_hz == original_hz);
    key(&ctx, SOLAR_OS_KEY_UP);
    rfscan.focus = 0; key(&ctx, SOLAR_OS_KEY_DOWN); busy = true;
    rfscan.focus = 0; key(&ctx, SOLAR_OS_KEY_UP);
    assert(!rfscan.scan.running && strstr(rfscan.message, "meshcore:test"));
    assert(rfscan.center_hz == original_hz + 250000U);
    busy = false; key(&ctx, SOLAR_OS_KEY_UP); key(&ctx, '\t');
    assert(rfscan.band && rfscan.scan.running && !strcmp(buttons[3], "BW"));
    assert(strstr(header_line, "LIVE RX paused"));
    rfscan.focus = 3; key(&ctx, SOLAR_OS_KEY_UP);
    assert(rfscan.scan.config.bandwidth_hz == 62500 && rfscan.scan.running);
    key(&ctx, '\t');
    assert(!rfscan.band && !strcmp(buttons[3], "Freq"));
    assert(worker.view.config.bandwidth_hz == 0);

    /* Enter opens a prefilled popup, cancellation preserves the running scan,
     * and custom steps remain useful for subsequent frequency adjustments. */
    rfscan.focus = 2; key(&ctx, '\r');
    assert(rfscan.edit == RFSCAN_EDIT_STEP && rfscan.scan.running);
    key(&ctx, SOLAR_OS_KEY_PAGE_UP); assert(rfscan_y_spans[rfscan.y_zoom] == 100);
    input(&ctx, "12.345\r");
    assert(rfscan.edit == RFSCAN_EDIT_NONE && rfscan.step_hz == 12345 && rfscan.scan.running);
    key(&ctx, '\r'); input(&ctx, "0\r");
    assert(rfscan.edit == RFSCAN_EDIT_STEP && rfscan.edit_error[0] && rfscan.step_hz == 12345);
    key(&ctx, SOLAR_OS_KEY_ESCAPE); assert(rfscan.scan.running && !ctx.exit_requested);
    rfscan.focus = 3; key(&ctx, '\r');
    assert(rfscan.edit == RFSCAN_EDIT_FREQUENCY);
    input(&ctx, "868.123456\r");
    assert(rfscan.edit == RFSCAN_EDIT_NONE && rfscan.center_hz == 868123456 && rfscan.scan.running);
    assert(worker.view.config.frequency_hz == 868123456);
    rfscan.focus = 3; key(&ctx, SOLAR_OS_KEY_UP);
    assert(rfscan.center_hz == 868135801 && rfscan.scan.running);
    key(&ctx, '\r'); input(&ctx, "868.25"); fail_next_start = true; key(&ctx, '\r');
    assert(rfscan.edit == RFSCAN_EDIT_FREQUENCY && rfscan.edit_error[0]);
    assert(rfscan.center_hz == 868135801 && worker.view.config.frequency_hz == 868135801);
    assert(rfscan.scan.running); /* Rejected setting rolls back and resumes the old scan. */
    key(&ctx, SOLAR_OS_KEY_ESCAPE);
    rfscan.focus = 1; key(&ctx, '\r'); input(&ctx, "2.5\r");
    assert(rfscan.window_ms == 2500 && rfscan.edit == RFSCAN_EDIT_NONE && rfscan.scan.running);
    key(&ctx, SOLAR_OS_KEY_UP); assert(rfscan.window_ms == 10000 && rfscan.scan.running);

    key(&ctx, '\t');
    rfscan.focus = 1; key(&ctx, '\r'); input(&ctx, "867.5 868.5\r");
    assert(rfscan.range == 4 && rfscan.scan.config.start_hz == 867500000 && rfscan.scan.config.end_hz == 868500000);
    assert(rfscan.scan.running);
    rfscan.focus = 2; key(&ctx, '\r'); input(&ctx, "10\r");
    assert(rfscan.step_hz == 10000 && rfscan.edit == RFSCAN_EDIT_NONE);
    key(&ctx, '\r'); input(&ctx, "1\r");
    assert(rfscan.edit_error[0] && rfscan.step_hz == 10000);
    key(&ctx, SOLAR_OS_KEY_ESCAPE);
    rfscan.focus = 3; key(&ctx, '\r'); input(&ctx, "78.2\r");
    assert(rfscan.bandwidth_hz == 78200 && rfscan.edit == RFSCAN_EDIT_NONE && rfscan.scan.running);
    assert(worker.view.config.bandwidth_hz == 78200 && worker.view.config.step_hz == 10000);
    unsigned stop_count = stops;
    tick(&ctx, 181000);
    assert(rfscan.scan.running && stops == stop_count); /* No idle cutoff. */
    key(&ctx, SOLAR_OS_KEY_PAGE_UP);
    assert(rfscan_y_spans[rfscan.y_zoom] == 50 && rfscan.scan.running && stops == stop_count);

    /* A brief strong bin between redraws triggers even if the latest bin is
     * weak. Held history and repeated UI ticks cannot retrigger stale energy. */
    worker.view.latest = -110; worker.view.recent_peak = -80;
    worker.view.recent_peak_frequency_hz = 868250000;
    worker.view.generation++;
    tick(&ctx, 25); assert(beeps == 1 && rfscan.scan.running);
    assert(rfscan.last_beep_hz == 868250000 && !strcmp(threshold_line, "Threshold: >=-90dBm Triggered: 868.250000MHz"));
    worker.view.recent_peak = -70;
    tick(&ctx, 25); assert(beeps == 1); /* Cooldown. */
    worker.view.latest = -20; worker.view.peak = -10;
    tick(&ctx, 1100); assert(beeps == 1); /* No fresh sample. */
    assert(rfscan.last_beep_hz == 868250000);
    worker.view.recent_peak = -91;
    tick(&ctx, 25); assert(beeps == 1); /* Below threshold. */
    worker.view.recent_peak = -90;
    worker.view.recent_peak_frequency_hz = 868375000;
    tick(&ctx, 25); assert(beeps == 2); /* Equal to threshold triggers too. */
    assert(rfscan.last_beep_hz == 868375000 && strstr(threshold_line, "868.375000MHz"));
    rfscan.focus = 4; key(&ctx, SOLAR_OS_KEY_UP);
    assert(rfscan.threshold_dbm == -88 && stops == stop_count && cancellations == 1);
    key(&ctx, '\r'); assert(rfscan.edit == RFSCAN_EDIT_THRESHOLD);
    input(&ctx, "off\r"); assert(rfscan.threshold_dbm == SOLAR_OS_RADIO_SCAN_EMPTY);
    worker.view.recent_peak = -20; tick(&ctx, 1100); assert(beeps == 2);
    key(&ctx, '\r'); input(&ctx, "-87\r");
    assert(rfscan.threshold_dbm == -87 && rfscan.scan.running);
    key(&ctx, SOLAR_OS_KEY_UP); assert(rfscan.threshold_dbm == -85);
    worker.view.recent_peak = -81; worker.view.recent_peak_frequency_hz = 868500000;
    tick(&ctx, 25); assert(beeps == 3 && rfscan.last_beep_hz == 868500000);
    audio_error = ESP_ERR_INVALID_STATE;
    worker.view.recent_peak = -80; tick(&ctx, 1100);
    assert(beeps == 3 && rfscan.beep_id == last_beep && rfscan.last_beep_hz == 868500000);
    audio_error = ESP_ERR_NOT_SUPPORTED;
    worker.view.recent_peak = -80; tick(&ctx, 1100);
    assert(rfscan.scan.running && strstr(rfscan.message, "Beep:") && beeps == 3);
    rfscan_suspend(&ctx); assert(cancellations == 2 && !rfscan.scan.running);
    tick(&ctx, 1100); assert(beeps == 3);
    rfscan_resume(&ctx);
    unsigned start_count = starts;
    rfscan.focus = 3; key(&ctx, SOLAR_OS_KEY_UP); key(&ctx, '\t');
    rfscan.focus = 3; key(&ctx, '\r'); input(&ctx, "868.5\r");
    assert(!rfscan.scan.running && starts == start_count && rfscan.center_hz == 868500000);
    assert(strstr(header_line, "STOPPED") && !strstr(header_line, "LIVE"));
    key(&ctx, SOLAR_OS_KEY_APP_EXIT); assert(ctx.exit_requested);
    rfscan_close(&ctx); free(rfscan_state);
    puts("rfscan: controls, popup values/cancel/validation, uninterrupted scans and fresh threshold beeps: ok");
}
