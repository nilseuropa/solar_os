#include "solar_os_rfscan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "esp_timer.h"
#include "solar_os_audio.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_radio_scan.h"
#include "solar_os_tui_widgets.h"

typedef enum {
    RFSCAN_EDIT_NONE,
    RFSCAN_EDIT_FREQUENCY,
    RFSCAN_EDIT_STEP,
    RFSCAN_EDIT_BANDWIDTH,
    RFSCAN_EDIT_RANGE,
    RFSCAN_EDIT_TIME,
    RFSCAN_EDIT_THRESHOLD,
} rfscan_edit_t;

typedef struct {
    uint32_t center_hz, window_ms, step_hz, bandwidth_hz;
    uint32_t range_start_hz, range_end_hz;
    unsigned range;
    bool band, channel_frequency;
} rfscan_settings_t;

typedef struct {
    solar_os_radio_scan_t scan;
    solar_os_radio_scan_worker_t *worker;
    solar_os_radio_info_t radio;
    uint32_t center_hz;
    uint32_t window_ms;
    uint32_t next_render;
    uint32_t rendered_generation;
    uint32_t next_beep;
    uint32_t beep_id;
    uint32_t last_beep_hz;
    int16_t threshold_dbm;
    int16_t average_dbm;
    int16_t y_center_dbm;
    bool y_fixed;
    unsigned y_zoom;
    unsigned range;
    uint32_t step_hz;
    uint32_t bandwidth_hz;
    uint32_t range_start_hz;
    uint32_t range_end_hz;
    unsigned focus;
    bool band;
    bool channel_frequency;
    bool suspended;
    rfscan_edit_t edit;
    bool edit_replace;
    solar_os_tui_input_state_t edit_cursor;
    char edit_text[48];
    char edit_error[48];
    int pointer_x;
    int pointer_y;
    char message[80];
} rfscan_state_t;

static void *rfscan_state;
#define rfscan (*(rfscan_state_t *)rfscan_state)

static const uint32_t rfscan_steps[] = {25000, 62500, 125000, 250000};
static const uint32_t rfscan_bandwidths[] = {0, 62500, 125000, 250000};
static const uint32_t rfscan_windows[] = {10000, 60000, 600000, 3600000};
static const unsigned rfscan_y_spans[] = {5, 10, 25, 50, 100, 200};

static uint32_t rfscan_next_value(uint32_t value, const uint32_t *values, size_t count, int direction)
{
    if (direction < 0) {
        for (size_t i = count; i > 0; i--) if (values[i - 1] < value) return values[i - 1];
        return values[count - 1];
    }
    for (size_t i = 0; i < count; i++) if (values[i] > value) return values[i];
    return values[0];
}

static uint32_t rfscan_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void rfscan_result(esp_err_t err)
{
    if (err == ESP_OK) {
        rfscan.message[0] = '\0';
    } else if (rfscan.radio.claimed && err == ESP_ERR_INVALID_STATE) {
        snprintf(rfscan.message, sizeof(rfscan.message), "Busy: %s owns %s",
                 rfscan.radio.owner, rfscan.radio.name);
    } else {
        snprintf(rfscan.message, sizeof(rfscan.message), "%s", esp_err_to_name(err));
    }
}

static void rfscan_cancel_beep(void)
{
    if (rfscan.beep_id != 0) {
        (void)solar_os_audio_tone_cancel(rfscan.beep_id);
        rfscan.beep_id = 0;
    }
}

static void rfscan_alert(uint32_t now)
{
    if (!rfscan.scan.running || rfscan.threshold_dbm == SOLAR_OS_RADIO_SCAN_EMPTY ||
        rfscan.scan.recent_peak == SOLAR_OS_RADIO_SCAN_EMPTY ||
        rfscan.scan.recent_peak < rfscan.threshold_dbm ||
        (int32_t)(now - rfscan.next_beep) < 0) return;
    rfscan.next_beep = now + 1000U;
    const solar_os_audio_tone_step_t step = {.frequency_hz = 1200, .duration_ms = 70};
    const solar_os_audio_tone_request_t request = {
        .steps = &step, .step_count = 1, .volume = SOLAR_OS_AUDIO_VOLUME_GLOBAL,
        .drop_if_busy = true,
    };
    uint32_t request_id = 0;
    esp_err_t err = solar_os_audio_tone_enqueue(&request, &request_id);
    if (err == ESP_OK) {
        rfscan.beep_id = request_id;
        rfscan.last_beep_hz = rfscan.scan.recent_peak_frequency_hz;
        rfscan.next_render = now;
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        snprintf(rfscan.message, sizeof(rfscan.message), "Beep: %s", esp_err_to_name(err));
    }
}

static bool rfscan_stop(void)
{
    rfscan_cancel_beep();
    esp_err_t err = solar_os_radio_scan_worker_close(&rfscan.worker, &rfscan.scan);
    rfscan.scan.running = false;
    if (err != ESP_OK) {
        snprintf(rfscan.message, sizeof(rfscan.message), "%s: %s",
            rfscan.worker != NULL ? "Stopping" : "Restore failed", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static void rfscan_parameters(void)
{
    static const uint32_t spans[] = {250000, 1000000, 2000000};
    solar_os_radio_scan_config_t *config = &rfscan.scan.config;
    config->mode = rfscan.band ? SOLAR_OS_RADIO_SCAN_BAND : SOLAR_OS_RADIO_SCAN_CHANNEL;
    config->interval_ms = rfscan.window_ms / SOLAR_OS_RADIO_SCAN_HISTORY;
    config->bandwidth_hz = rfscan.band ? rfscan.bandwidth_hz : 0;
    config->step_hz = rfscan.step_hz;
    config->frequency_hz = !rfscan.band && rfscan.channel_frequency ? rfscan.center_hz : 0;
    if (!rfscan.band) {
        config->start_hz = config->end_hz = rfscan.center_hz;
        return;
    }
    if (rfscan.range < 3) {
        config->start_hz = rfscan.center_hz > spans[rfscan.range] ?
            rfscan.center_hz - spans[rfscan.range] : rfscan.center_hz;
        config->end_hz = rfscan.center_hz > UINT32_MAX - spans[rfscan.range] ?
            UINT32_MAX : rfscan.center_hz + spans[rfscan.range];
    } else if (rfscan.range == 4) {
        config->start_hz = rfscan.range_start_hz;
        config->end_hz = rfscan.range_end_hz;
    } else if (rfscan.center_hz < 500000000U) {
        config->start_hz = 433050000U;
        config->end_hz = 434790000U;
    } else if (rfscan.center_hz < 890000000U) {
        config->start_hz = 863000000U;
        config->end_hz = 870000000U;
    } else {
        config->start_hz = 902000000U;
        config->end_hz = 928000000U;
    }
    if ((config->end_hz - config->start_hz) / config->step_hz >= SOLAR_OS_RADIO_SCAN_BINS) {
        uint32_t minimum = (uint32_t)(((uint64_t)config->end_hz - config->start_hz +
            SOLAR_OS_RADIO_SCAN_BINS - 2U) / (SOLAR_OS_RADIO_SCAN_BINS - 1U));
        rfscan.step_hz = minimum;
        for (size_t i = 0; i < 4; i++) {
            if (rfscan_steps[i] >= minimum) { rfscan.step_hz = rfscan_steps[i]; break; }
        }
        config->step_hz = rfscan.step_hz;
    }
    /* Display the last actual sample frequency rather than an unsampled edge. */
    config->end_hz = config->start_hz +
        (config->end_hz - config->start_hz) / config->step_hz * config->step_hz;
}

static bool rfscan_select(const char *name)
{
    solar_os_radio_info_t info;
    solar_os_radio_status_t status;
    esp_err_t err = solar_os_radio_get_info(name, &info);
    if (err == ESP_OK) err = solar_os_radio_get_status(name, &status);
    if (err != ESP_OK) { rfscan_result(err); return false; }
    rfscan.radio = info;
    rfscan.center_hz = status.config.frequency_hz;
    rfscan.channel_frequency = false;
    rfscan.scan.receiver = status.config;
    strlcpy(rfscan.scan.name, name, sizeof(rfscan.scan.name));
    rfscan_parameters();
    return true;
}

static bool rfscan_begin(void)
{
    if (rfscan.radio.name[0] == '\0') {
        strlcpy(rfscan.message, "No attached radio", sizeof(rfscan.message));
        return false;
    }
    if (rfscan.worker != NULL && !rfscan_stop()) return false;
    solar_os_radio_status_t status;
    esp_err_t err = solar_os_radio_get_info(rfscan.radio.name, &rfscan.radio);
    if (err == ESP_OK) err = solar_os_radio_get_status(rfscan.radio.name, &status);
    if (err != ESP_OK) { rfscan_result(err); return false; }
    rfscan.scan.receiver = status.config;
    if (!rfscan.band && !rfscan.channel_frequency) rfscan.center_hz = status.config.frequency_hz;
    rfscan_parameters();
    solar_os_radio_scan_config_t config = rfscan.scan.config;
    err = solar_os_radio_scan_worker_open(&rfscan.worker, rfscan.radio.name, &config);
    rfscan_result(err);
    if (err != ESP_OK) return false;
    rfscan.scan.running = true;
    if (solar_os_radio_scan_worker_copy(rfscan.worker, &rfscan.scan) == ESP_OK)
        rfscan_alert(rfscan_now());
    return true;
}

static rfscan_settings_t rfscan_settings(void)
{
    return (rfscan_settings_t){
        .center_hz = rfscan.center_hz, .window_ms = rfscan.window_ms,
        .step_hz = rfscan.step_hz, .bandwidth_hz = rfscan.bandwidth_hz,
        .range_start_hz = rfscan.range_start_hz, .range_end_hz = rfscan.range_end_hz,
        .range = rfscan.range, .band = rfscan.band, .channel_frequency = rfscan.channel_frequency,
    };
}

static bool rfscan_apply_settings(const rfscan_settings_t *before, bool was_running)
{
    rfscan_parameters();
    if (!was_running) { solar_os_radio_scan_clear(&rfscan.scan); return true; }
    if (rfscan_begin()) return true;
    /* A rejected setting must not strand a working receiver. Restore the
     * previous selection and continue it when ownership still permits. */
    char error[sizeof(rfscan.message)];
    strlcpy(error, rfscan.message, sizeof(error));
    rfscan.center_hz = before->center_hz; rfscan.window_ms = before->window_ms;
    rfscan.step_hz = before->step_hz; rfscan.bandwidth_hz = before->bandwidth_hz;
    rfscan.range_start_hz = before->range_start_hz; rfscan.range_end_hz = before->range_end_hz;
    rfscan.range = before->range; rfscan.band = before->band;
    rfscan.channel_frequency = before->channel_frequency;
    rfscan_parameters();
    if (rfscan_begin()) strlcpy(rfscan.message, error, sizeof(rfscan.message));
    return false;
}

static void rfscan_center_y(void)
{
    solar_os_radio_scan_t *scan = &rfscan.scan;
    int32_t sum = 0;
    size_t count = 0;
    if (rfscan.band) {
        for (size_t i = 0; i < scan->bins; i++) {
            if (scan->levels[i] == SOLAR_OS_RADIO_SCAN_EMPTY) continue;
            sum += scan->levels[i];
            count++;
        }
    } else if (scan->history_count) {
        const uint32_t newest = scan->history_ms[
            (scan->history_head + SOLAR_OS_RADIO_SCAN_HISTORY - 1U) % SOLAR_OS_RADIO_SCAN_HISTORY];
        for (size_t i = 0; i < scan->history_count; i++) {
            size_t index = (scan->history_head + SOLAR_OS_RADIO_SCAN_HISTORY - scan->history_count + i) %
                SOLAR_OS_RADIO_SCAN_HISTORY;
            if (newest - scan->history_ms[index] > rfscan.window_ms ||
                scan->history[index] == SOLAR_OS_RADIO_SCAN_EMPTY) continue;
            sum += scan->history[index];
            count++;
        }
    }
    if (count) {
        const int32_t rounded = sum + (sum < 0 ? -1 : 1) * (int32_t)(count / 2U);
        rfscan.average_dbm = (int16_t)(rounded / (int32_t)count);
    } else if (scan->latest != SOLAR_OS_RADIO_SCAN_EMPTY) rfscan.average_dbm = scan->latest;
    if (!rfscan.y_fixed) rfscan.y_center_dbm = rfscan.average_dbm;
}

static unsigned rfscan_threshold_step(void)
{
    unsigned step = rfscan_y_spans[rfscan.y_zoom] / 20U;
    return step > 0 ? step : 1U;
}

static void rfscan_zoom(int direction)
{
    rfscan_center_y();
    if (direction < 0 && rfscan.y_zoom > 0) rfscan.y_zoom--;
    if (direction > 0 && rfscan.y_zoom + 1U < sizeof(rfscan_y_spans) / sizeof(rfscan_y_spans[0])) rfscan.y_zoom++;
    rfscan.y_center_dbm = rfscan.average_dbm;
    rfscan.y_fixed = true;
}

static bool rfscan_y_visible(int dbm)
{
    const int distance = 2 * ((int)dbm - rfscan.y_center_dbm);
    const int span = (int)rfscan_y_spans[rfscan.y_zoom];
    return distance >= -span && distance <= span;
}

static int rfscan_y(int dbm, int top, int height)
{
    const int span = (int)rfscan_y_spans[rfscan.y_zoom];
    int distance = span + 2 * ((int)rfscan.y_center_dbm - dbm);
    if (distance < 0) distance = 0;
    if (distance > span * 2) distance = span * 2;
    return top + (height - 1) * distance / (span * 2);
}

static void rfscan_button(solar_os_gfx_t *gfx, int x, int y, int width,
                           const char *text, bool selected)
{
    /* Match the standard transport controls: outlined when idle, solid inverse
     * when selected. Control states must remain distinct on a 1-bit surface. */
    solar_os_gfx_set_color(gfx, selected ? SOLAR_OS_GFX_COLOR_BLACK : SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, width, 21);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, width, 21);
    solar_os_gfx_set_color(gfx, selected ? SOLAR_OS_GFX_COLOR_WHITE : SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(gfx, x + (width - (int)solar_os_gfx_text_width(gfx, text)) / 2, y + 14, text);
}

static void rfscan_popup_box(solar_os_gfx_t *gfx, int *x, int *y, int *width)
{
    const int screen_width = (int)solar_os_gfx_width(gfx);
    *width = screen_width > 300 ? 280 : screen_width - 16;
    *x = (screen_width - *width) / 2;
    *y = ((int)solar_os_gfx_height(gfx) - 116) / 2;
}

static size_t rfscan_popup_cells(solar_os_gfx_t *gfx)
{
    int x, y, width;
    rfscan_popup_box(gfx, &x, &y, &width);
    return width > 30 ? (size_t)(width - 24) / 6U : 1U;
}

static void rfscan_render_popup(solar_os_gfx_t *gfx)
{
    if (rfscan.edit == RFSCAN_EDIT_NONE) return;
    static const char *titles[] = {"", "Frequency (MHz)", "Step (kHz)",
        "Bandwidth (kHz; 0=current)", "Range (MHz: start end)",
        "Time window (seconds)", "Threshold (dBm or off)"};
    int x, y, width;
    rfscan_popup_box(gfx, &x, &y, &width);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, width, 116);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, width, 116);
    solar_os_gfx_text(gfx, x + 8, y + 17, titles[rfscan.edit]);
    solar_os_gfx_rect(gfx, x + 7, y + 26, width - 14, 23);
    if (rfscan.edit_replace) solar_os_gfx_fill_rect(gfx, x + 8, y + 27, width - 16, 21);
    solar_os_gfx_set_color(gfx, rfscan.edit_replace ? SOLAR_OS_GFX_COLOR_WHITE : SOLAR_OS_GFX_COLOR_BLACK);
    size_t cells = rfscan_popup_cells(gfx);
    char visible[48];
    strlcpy(visible, rfscan.edit_text + rfscan.edit_cursor.view, sizeof(visible));
    if (cells < sizeof(visible)) visible[cells] = '\0';
    solar_os_gfx_text(gfx, x + 11, y + 42, visible);
    if (!rfscan.edit_replace) {
        size_t cursor = rfscan.edit_cursor.cursor - rfscan.edit_cursor.view;
        if (cursor < sizeof(visible)) visible[cursor] = '\0';
        int caret = x + 11 + (int)solar_os_gfx_text_width(gfx, visible);
        solar_os_gfx_line(gfx, caret, y + 30, caret, y + 44);
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(gfx, x + 8, y + 65, rfscan.edit_error[0] ? rfscan.edit_error : "Type a value; Enter applies, Esc cancels");
    rfscan_button(gfx, x + 7, y + 86, (width - 21) / 2, "Enter: OK", true);
    rfscan_button(gfx, x + 14 + (width - 21) / 2, y + 86, (width - 21) / 2, "Esc: Cancel", false);
}

static void rfscan_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || rfscan.suspended) return;
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    solar_os_radio_scan_t *scan = &rfscan.scan;
    rfscan_center_y();
    char text[96];
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    const char *state = !scan->running ? "STOPPED" : rfscan.band ? "LIVE RX paused" :
        scan->restore ? "LIVE owns RX" : "LIVE observe RX";
    snprintf(text, sizeof(text), "%s  %s  %s", rfscan.radio.name[0] ? rfscan.radio.name : "No radio",
             rfscan.radio.driver, state);
    if (!rfscan.band) {
        const size_t used = strlen(text);
        snprintf(text + used, sizeof(text) - used, "  Time: %gs", rfscan.window_ms / 1000.0);
    }
    solar_os_gfx_text(gfx, 4, 12, text);
    rfscan_button(gfx, 4, 18, width / 2 - 6, "Channel", !rfscan.band);
    rfscan_button(gfx, width / 2 + 2, 18, width / 2 - 6, "Band", rfscan.band);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    const uint32_t bandwidth = !scan->running && scan->config.bandwidth_hz ?
        scan->config.bandwidth_hz : scan->receiver.rx_bandwidth_hz;
    if (rfscan.band) {
        snprintf(text, sizeof(text), "%.3f-%.3fMHz BW: %.3fkHz Step: %gkHz",
                 scan->config.start_hz / 1000000.0, scan->config.end_hz / 1000000.0,
                 bandwidth / 1000.0, rfscan.step_hz / 1000.0);
    } else {
        snprintf(text, sizeof(text), "Freq: %.6fMHz BW: %.3fkHz Step: %gkHz",
                 (scan->running ? scan->receiver.frequency_hz : rfscan.center_hz) / 1000000.0,
                 bandwidth / 1000.0, rfscan.step_hz / 1000.0);
    }
    solar_os_gfx_text(gfx, 4, 52, text);
    char threshold[64];
    if (rfscan.threshold_dbm == SOLAR_OS_RADIO_SCAN_EMPTY) strlcpy(threshold, "Threshold: Off", sizeof(threshold));
    else snprintf(threshold, sizeof(threshold), "Threshold: >=%ddBm", rfscan.threshold_dbm);
    const size_t used = strlen(threshold);
    if (rfscan.last_beep_hz) {
        snprintf(threshold + used, sizeof(threshold) - used, " Triggered: %.6fMHz", rfscan.last_beep_hz / 1000000.0);
    } else strlcpy(threshold + used, " Triggered: --", sizeof(threshold) - used);
    solar_os_gfx_text(gfx, 4, 76, threshold);
    if (rfscan.message[0]) strlcpy(text, rfscan.message, sizeof(text));
    else if (scan->latest != SOLAR_OS_RADIO_SCAN_EMPTY) {
        snprintf(text, sizeof(text), "Now: %d Avg: %d Min: %d Peak: %d dBm",
            scan->latest, rfscan.average_dbm, scan->floor, scan->peak);
    } else {
        strlcpy(text, "Waiting for receiver energy", sizeof(text));
    }
    solar_os_gfx_text(gfx, 4, 64, text);

    const int left = 29, plot_width = width - left - 5;
    const int top = 86, available = height - top - 32;
    if (plot_width > 1 && available > 12) {
        const int plot_height = rfscan.band ? available / 2 : available;
        const int span = (int)rfscan_y_spans[rfscan.y_zoom];
        const int tick = span <= 10 ? 1 : span <= 25 ? 5 : span <= 50 ? 10 : span <= 100 ? 25 : 50;
        const int low = rfscan.y_center_dbm - span / 2;
        const int high = rfscan.y_center_dbm + span / 2;
        int first = low - low % tick;
        if (first < low) first += tick;
        for (int level = first; level <= high; level += tick) {
            int y = rfscan_y(level, top, plot_height);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_DOTTED);
            solar_os_gfx_line(gfx, left, y, width - 5, y);
            solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);
            snprintf(text, sizeof(text), "%d", level);
            solar_os_gfx_text(gfx, 1, y + (y == top ? 6 : 0), text);
        }
        if (rfscan.threshold_dbm != SOLAR_OS_RADIO_SCAN_EMPTY && rfscan_y_visible(rfscan.threshold_dbm)) {
            const int y = rfscan_y(rfscan.threshold_dbm, top, plot_height);
            solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_DASHED);
            solar_os_gfx_line(gfx, left, y, width - 5, y);
            solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);
        }
        if (rfscan.band && scan->bins > 1) {
            for (size_t i = 1; i < scan->bins; i++) {
                int x0 = left + (i - 1) * (plot_width - 1) / (scan->bins - 1);
                int x1 = left + i * (plot_width - 1) / (scan->bins - 1);
                if (scan->peaks[i - 1] != SOLAR_OS_RADIO_SCAN_EMPTY && scan->peaks[i] != SOLAR_OS_RADIO_SCAN_EMPTY) {
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_DASHED);
                    solar_os_gfx_line(gfx, x0, rfscan_y(scan->peaks[i - 1], top, plot_height),
                                     x1, rfscan_y(scan->peaks[i], top, plot_height));
                    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);
                }
                if (scan->levels[i - 1] != SOLAR_OS_RADIO_SCAN_EMPTY && scan->levels[i] != SOLAR_OS_RADIO_SCAN_EMPTY) {
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                    solar_os_gfx_line(gfx, x0, rfscan_y(scan->levels[i - 1], top, plot_height),
                                     x1, rfscan_y(scan->levels[i], top, plot_height));
                }
            }
            const int waterfall_top = top + plot_height + 3;
            const int waterfall_height = available - plot_height - 3;
            const size_t visible_rows = waterfall_height < (int)SOLAR_OS_RADIO_SCAN_ROWS ?
                (size_t)waterfall_height : SOLAR_OS_RADIO_SCAN_ROWS;
            for (size_t row = 0; row < scan->rows && row < visible_rows; row++) {
                const int y0 = waterfall_top + row * waterfall_height / visible_rows;
                const int y1 = waterfall_top + (row + 1) * waterfall_height / visible_rows;
                for (size_t i = 0; i < scan->bins; i++) {
                    int level = scan->waterfall[row][i];
                    int shade = (-30 - level) * 16 / 100;
                    if (shade < 0) shade = 0;
                    if (shade > 16) shade = 16;
                    solar_os_gfx_set_color(gfx, solar_os_gfx_gray(shade));
                    int x0 = left + i * plot_width / scan->bins;
                    int x1 = left + (i + 1) * plot_width / scan->bins;
                    solar_os_gfx_fill_rect(gfx, x0, y0, x1 - x0, y1 - y0);
                }
            }
        } else if (!rfscan.band && scan->history_count) {
            uint32_t now = scan->history_ms[(scan->history_head + SOLAR_OS_RADIO_SCAN_HISTORY - 1U) % SOLAR_OS_RADIO_SCAN_HISTORY];
            int previous_x = -1, previous_y = 0;
            for (size_t i = 0; i < scan->history_count; i++) {
                size_t index = (scan->history_head + SOLAR_OS_RADIO_SCAN_HISTORY - scan->history_count + i) % SOLAR_OS_RADIO_SCAN_HISTORY;
                uint32_t age = now - scan->history_ms[index];
                if (age > rfscan.window_ms) continue;
                int x = left + (uint64_t)(rfscan.window_ms - age) * (plot_width - 1) / rfscan.window_ms;
                int y = rfscan_y(scan->history[index], top, plot_height);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                if (previous_x >= 0) solar_os_gfx_line(gfx, previous_x, previous_y, x, y);
                else solar_os_gfx_pixel(gfx, x, y);
                previous_x = x; previous_y = y;
            }
        }
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    const char *buttons[] = {scan->running ? "Stop" : "Start", rfscan.band ? "Range" : "Time", "Step", rfscan.band ? "BW" : "Freq", "Thresh"};
    for (unsigned i = 0; i < 5; i++) {
        rfscan_button(gfx, i * width / 5 + 2, height - 24, width / 5 - 4, buttons[i], rfscan.focus == i);
    }
    rfscan_render_popup(gfx);
    solar_os_gfx_present(gfx);
    rfscan.rendered_generation = scan->generation;
}

static unsigned rfscan_cycle(unsigned value, unsigned count, int direction)
{
    return direction < 0 ? (value + count - 1U) % count : (value + 1U) % count;
}

static void rfscan_threshold(int16_t dbm)
{
    rfscan_cancel_beep();
    rfscan.threshold_dbm = dbm;
    rfscan.next_beep = rfscan_now();
}

static void rfscan_edit_begin(unsigned action)
{
    rfscan.edit = action == 1 ? (rfscan.band ? RFSCAN_EDIT_RANGE : RFSCAN_EDIT_TIME) :
        action == 2 ? RFSCAN_EDIT_STEP : action == 3 ?
        (rfscan.band ? RFSCAN_EDIT_BANDWIDTH : RFSCAN_EDIT_FREQUENCY) : RFSCAN_EDIT_THRESHOLD;
    if (rfscan.edit == RFSCAN_EDIT_FREQUENCY)
        snprintf(rfscan.edit_text, sizeof(rfscan.edit_text), "%.6f", rfscan.center_hz / 1000000.0);
    else if (rfscan.edit == RFSCAN_EDIT_STEP || rfscan.edit == RFSCAN_EDIT_BANDWIDTH)
        snprintf(rfscan.edit_text, sizeof(rfscan.edit_text), "%.3f",
            (rfscan.edit == RFSCAN_EDIT_STEP ? rfscan.step_hz : rfscan.bandwidth_hz) / 1000.0);
    else if (rfscan.edit == RFSCAN_EDIT_RANGE)
        snprintf(rfscan.edit_text, sizeof(rfscan.edit_text), "%.6f %.6f",
            rfscan.scan.config.start_hz / 1000000.0, rfscan.scan.config.end_hz / 1000000.0);
    else if (rfscan.edit == RFSCAN_EDIT_TIME)
        snprintf(rfscan.edit_text, sizeof(rfscan.edit_text), "%.3f", rfscan.window_ms / 1000.0);
    else if (rfscan.threshold_dbm == SOLAR_OS_RADIO_SCAN_EMPTY)
        strlcpy(rfscan.edit_text, "off", sizeof(rfscan.edit_text));
    else snprintf(rfscan.edit_text, sizeof(rfscan.edit_text), "%d", rfscan.threshold_dbm);
    rfscan.edit_cursor = (solar_os_tui_input_state_t){.cursor = strlen(rfscan.edit_text)};
    rfscan.edit_error[0] = '\0';
    rfscan.edit_replace = true;
}

static bool rfscan_parse_value(const char *text, uint32_t scale, uint32_t minimum,
    uint32_t maximum, uint32_t *value, const char **rest)
{
    char *end;
    double number = strtod(text, &end) * scale;
    if (end == text || !(number >= minimum && number <= maximum)) return false;
    while (isspace((unsigned char)*end)) end++;
    if (rest != NULL) *rest = end;
    else if (*end) return false;
    *value = (uint32_t)(number + 0.5);
    return true;
}

static bool rfscan_edit_submit(void)
{
    uint32_t value = 0, end = 0;
    bool valid;
    if (rfscan.edit == RFSCAN_EDIT_THRESHOLD) {
        char *tail;
        long dbm = strtol(rfscan.edit_text, &tail, 10);
        while (isspace((unsigned char)*tail)) tail++;
        if (!strcasecmp(rfscan.edit_text, "off")) rfscan_threshold(SOLAR_OS_RADIO_SCAN_EMPTY);
        else if (tail != rfscan.edit_text && !*tail && dbm >= -130 && dbm <= -30)
            rfscan_threshold((int16_t)dbm);
        else {
            strlcpy(rfscan.edit_error, "Enter -130 to -30, or off", sizeof(rfscan.edit_error));
            return false;
        }
        return true;
    }
    if (rfscan.edit == RFSCAN_EDIT_RANGE) {
        const char *rest;
        valid = rfscan_parse_value(rfscan.edit_text, 1000000U, 1, UINT32_MAX, &value, &rest);
        if (valid && (*rest == ',' || *rest == '-')) rest++;
        valid = valid && rfscan_parse_value(rest, 1000000U, value, UINT32_MAX, &end, NULL) && end > value;
    } else {
        const bool frequency = rfscan.edit == RFSCAN_EDIT_FREQUENCY;
        valid = rfscan_parse_value(rfscan.edit_text, frequency ? 1000000U : 1000U,
            rfscan.edit == RFSCAN_EDIT_BANDWIDTH ? 0 : rfscan.edit == RFSCAN_EDIT_TIME ? 1000U : 1U,
            rfscan.edit == RFSCAN_EDIT_TIME ? 3600000U : UINT32_MAX, &value, NULL);
    }
    if (!valid) {
        strlcpy(rfscan.edit_error, rfscan.edit == RFSCAN_EDIT_RANGE ?
            "Enter start and end MHz, in order" : rfscan.edit == RFSCAN_EDIT_TIME ?
            "Enter 1 to 3600 seconds" : "Enter a valid positive value", sizeof(rfscan.edit_error));
        return false;
    }
    if (rfscan.edit == RFSCAN_EDIT_STEP && rfscan.band &&
        (rfscan.scan.config.end_hz - rfscan.scan.config.start_hz) / value >= SOLAR_OS_RADIO_SCAN_BINS) {
        strlcpy(rfscan.edit_error, "Step too small for this range", sizeof(rfscan.edit_error));
        return false;
    }
    const rfscan_settings_t before = rfscan_settings();
    const bool was_running = rfscan.scan.running;
    const bool receiver_change = !(rfscan.edit == RFSCAN_EDIT_STEP && !rfscan.band);
    if (receiver_change && !rfscan_stop()) {
        strlcpy(rfscan.edit_error, "Could not stop receiver", sizeof(rfscan.edit_error));
        return false;
    }
    if (rfscan.edit == RFSCAN_EDIT_FREQUENCY) {
        rfscan.center_hz = value;
        rfscan.channel_frequency = true;
    } else if (rfscan.edit == RFSCAN_EDIT_STEP) rfscan.step_hz = value;
    else if (rfscan.edit == RFSCAN_EDIT_BANDWIDTH) rfscan.bandwidth_hz = value;
    else if (rfscan.edit == RFSCAN_EDIT_RANGE) {
        rfscan.range = 4;
        rfscan.range_start_hz = value;
        rfscan.range_end_hz = end;
    } else rfscan.window_ms = value;
    if (receiver_change) {
        if (!rfscan_apply_settings(&before, was_running)) {
            strlcpy(rfscan.edit_error, rfscan.message, sizeof(rfscan.edit_error));
            return false;
        }
    } else {
        rfscan_parameters();
        if (!rfscan.scan.running) solar_os_radio_scan_clear(&rfscan.scan);
    }
    return true;
}

static void rfscan_edit_key(uint8_t ch, size_t cells)
{
    if (ch == '\r') ch = SOLAR_OS_KEY_ENTER;
    const bool input = (ch >= ' ' && ch < SOLAR_OS_KEY_UP) ||
        ch == '\b' || ch == 0x7fU || ch == 0x15U || ch == SOLAR_OS_KEY_DELETE;
    const bool navigation = ch == SOLAR_OS_KEY_LEFT || ch == SOLAR_OS_KEY_RIGHT ||
        ch == SOLAR_OS_KEY_HOME || ch == SOLAR_OS_KEY_END ||
        ch == SOLAR_OS_KEY_CTRL_LEFT || ch == SOLAR_OS_KEY_CTRL_RIGHT;
    if (rfscan.edit_replace && input) {
        rfscan.edit_text[0] = '\0';
        rfscan.edit_cursor = (solar_os_tui_input_state_t){0};
    }
    if (ch == 0x15U) {
        rfscan.edit_text[0] = '\0';
        rfscan.edit_cursor = (solar_os_tui_input_state_t){0};
    }
    if (input || navigation) rfscan.edit_replace = false;
    solar_os_tui_input_action_t action = solar_os_tui_input_key(rfscan.edit_text,
        sizeof(rfscan.edit_text), &rfscan.edit_cursor, ch, cells);
    if (action == SOLAR_OS_TUI_INPUT_CANCEL ||
        (action == SOLAR_OS_TUI_INPUT_SUBMIT && rfscan_edit_submit())) rfscan.edit = RFSCAN_EDIT_NONE;
    if (action == SOLAR_OS_TUI_INPUT_CHANGED) rfscan.edit_error[0] = '\0';
}

static void rfscan_action(unsigned action, int direction)
{
    if (action == 0) {
        if ((direction > 0 && rfscan.scan.running) ||
            (direction < 0 && !rfscan.scan.running)) return;
        if (rfscan.scan.running) (void)rfscan_stop();
        else rfscan_begin();
        return;
    }
    if (direction == 0) { rfscan_edit_begin(action); return; }
    if (!rfscan.band && action == 2) {
        rfscan.step_hz = rfscan_next_value(rfscan.step_hz, rfscan_steps, 4, direction);
        rfscan_parameters();
        return;
    }
    if (action == 4) {
        int value = rfscan.threshold_dbm == SOLAR_OS_RADIO_SCAN_EMPTY ?
            (direction < 0 ? -30 : -130) : rfscan.threshold_dbm +
                (direction < 0 ? -(int)rfscan_threshold_step() : (int)rfscan_threshold_step());
        rfscan_threshold(value < -130 || value > -30 ? SOLAR_OS_RADIO_SCAN_EMPTY : value);
        return;
    }
    if (!rfscan.band && !rfscan.channel_frequency) rfscan.center_hz = rfscan.scan.receiver.frequency_hz;
    const rfscan_settings_t before = rfscan_settings();
    const bool was_running = rfscan.scan.running;
    int64_t frequency = rfscan.center_hz;
    if (action == 3 && !rfscan.band) {
        frequency += direction < 0 ? -(int64_t)rfscan.step_hz : rfscan.step_hz;
        if (frequency <= 0 || frequency > UINT32_MAX) {
            strlcpy(rfscan.message, "Frequency out of range", sizeof(rfscan.message));
            return;
        }
    }
    if (!rfscan_stop()) return;
    if (action == 1) {
        if (rfscan.band) rfscan.range = rfscan.range >= 4 ?
            (direction < 0 ? 3U : 0U) : rfscan_cycle(rfscan.range, 4, direction);
        else {
            rfscan.window_ms = rfscan_next_value(rfscan.window_ms, rfscan_windows, 4, direction);
        }
    } else if (action == 3 && !rfscan.band) {
        rfscan.center_hz = (uint32_t)frequency;
        rfscan.channel_frequency = true;
    } else if (action == 2) rfscan.step_hz = rfscan_next_value(rfscan.step_hz, rfscan_steps, 4, direction);
    else if (action == 3) rfscan.bandwidth_hz = rfscan_next_value(rfscan.bandwidth_hz, rfscan_bandwidths, 4, direction);
    (void)rfscan_apply_settings(&before, was_running);
}

static void rfscan_set_mode(bool band)
{
    if (rfscan.band == band) return;
    const rfscan_settings_t before = rfscan_settings();
    const bool was_running = rfscan.scan.running;
    if (!rfscan_stop()) return;
    rfscan.band = band;
    (void)rfscan_apply_settings(&before, was_running);
}

static esp_err_t rfscan_start(solar_os_context_t *ctx)
{
    if (solar_os_context_gfx(ctx) == NULL) return ESP_ERR_NOT_SUPPORTED;
    memset(&rfscan, 0, sizeof(rfscan));
    solar_os_radio_scan_init(&rfscan.scan);
    rfscan.window_ms = 10000;
    rfscan.range = 2;
    rfscan.step_hz = 125000U;
    rfscan.threshold_dbm = -90;
    rfscan.average_dbm = -80;
    rfscan.y_center_dbm = -80;
    rfscan.y_zoom = 4; /* 100 dB span until changed with Page Up/Down. */
    rfscan.next_beep = rfscan_now();
    if (solar_os_context_argc(ctx) > 1) {
        if (!rfscan_select(solar_os_context_argv(ctx, 1))) return ESP_ERR_NOT_FOUND;
    } else {
        solar_os_radio_info_t info;
        if (solar_os_radio_get(0, &info)) (void)rfscan_select(info.name);
    }
    solar_os_context_set_graphics_active(ctx, true);
    if (rfscan.radio.name[0]) rfscan_begin();
    rfscan_render(ctx);
    return ESP_OK;
}

static void rfscan_close(solar_os_context_t *ctx)
{
    (void)rfscan_stop();
    solar_os_context_set_graphics_active(ctx, false);
}

static void rfscan_suspend(solar_os_context_t *ctx)
{
    (void)rfscan_stop();
    rfscan.suspended = true;
    solar_os_context_set_graphics_active(ctx, false);
}

static void rfscan_resume(solar_os_context_t *ctx)
{
    if (rfscan.worker != NULL) (void)rfscan_stop();
    rfscan.suspended = false;
    solar_os_context_set_graphics_active(ctx, true);
    rfscan_render(ctx);
}

static bool rfscan_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL || rfscan.suspended) return false;
    uint32_t now = rfscan_now();
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (rfscan.worker != NULL) {
            esp_err_t err = solar_os_radio_scan_worker_copy(rfscan.worker, &rfscan.scan);
            if (err == ESP_OK) rfscan_alert(now);
            if (err == ESP_OK && !rfscan.band && !rfscan.channel_frequency)
                rfscan.center_hz = rfscan.scan.receiver.frequency_hz;
            if (err == ESP_OK && !rfscan.scan.running) {
                esp_err_t sample_error = rfscan.scan.error;
                (void)rfscan_stop();
                rfscan_result(sample_error);
            }
        }
        if (rfscan.scan.generation == rfscan.rendered_generation) return false;
        if ((int32_t)(now - rfscan.next_render) < 0) return false;
        rfscan.next_render = now + 400U;
    } else if (event->type == SOLAR_OS_EVENT_CHAR) {
        uint8_t ch = (uint8_t)event->data.ch;
        if (ch == SOLAR_OS_KEY_APP_EXIT) {
            solar_os_context_finish(ctx, 0, NULL);
            return true;
        }
        if (rfscan.edit != RFSCAN_EDIT_NONE) {
            solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
            rfscan_edit_key(ch, rfscan_popup_cells(gfx));
            rfscan_render(ctx);
            return true;
        }
        if (ch == 'q' || ch == SOLAR_OS_KEY_ESCAPE) {
            solar_os_context_finish(ctx, 0, NULL);
            return true;
        }
        if (ch == SOLAR_OS_KEY_LEFT) rfscan.focus = (rfscan.focus + 4) % 5;
        else if (ch == SOLAR_OS_KEY_RIGHT) rfscan.focus = (rfscan.focus + 1) % 5;
        else if (ch == '\t') rfscan_set_mode(!rfscan.band);
        else if (ch == SOLAR_OS_KEY_UP) rfscan_action(rfscan.focus, 1);
        else if (ch == SOLAR_OS_KEY_DOWN) rfscan_action(rfscan.focus, -1);
        else if (ch == SOLAR_OS_KEY_PAGE_UP) rfscan_zoom(-1);
        else if (ch == SOLAR_OS_KEY_PAGE_DOWN) rfscan_zoom(1);
        else if (ch == '\n' || ch == '\r') rfscan_action(rfscan.focus, 0);
        else if (ch == ' ') rfscan_action(0, 0);
        else if (ch == 'r') rfscan_action(1, 1);
        else if (ch == 'f' && !rfscan.band) rfscan_action(3, 1);
        else if (ch == 's') rfscan_action(2, 1);
        else if (ch == 'b' && rfscan.band) rfscan_action(3, 1);
        else if (ch == 't') rfscan_action(4, 1);
        else if (ch == 'c') {
            if (rfscan.worker != NULL) {
                (void)solar_os_radio_scan_worker_clear(rfscan.worker);
                (void)solar_os_radio_scan_worker_copy(rfscan.worker, &rfscan.scan);
            } else solar_os_radio_scan_clear(&rfscan.scan);
        }
        else return false;
    } else if (event->type == SOLAR_OS_EVENT_POINTER) {
        const solar_os_input_pointer_event_t *p = &event->data.pointer;
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        const int width = (int)solar_os_gfx_width(gfx), height = (int)solar_os_gfx_height(gfx);
        if (p->mode == SOLAR_OS_INPUT_POINTER_ABSOLUTE) { rfscan.pointer_x = p->x; rfscan.pointer_y = p->y; }
        else { rfscan.pointer_x += p->delta_x; rfscan.pointer_y += p->delta_y; }
        if (rfscan.pointer_x < 0) rfscan.pointer_x = 0;
        if (rfscan.pointer_x >= width) rfscan.pointer_x = width - 1;
        if (rfscan.pointer_y < 0) rfscan.pointer_y = 0;
        if (rfscan.pointer_y >= height) rfscan.pointer_y = height - 1;
        if (p->action != SOLAR_OS_INPUT_POINTER_PRESS) return false;
        if (rfscan.edit != RFSCAN_EDIT_NONE) {
            int x, y, popup_width;
            rfscan_popup_box(gfx, &x, &y, &popup_width);
            if (rfscan.pointer_y >= y + 86 && rfscan.pointer_y < y + 107 &&
                rfscan.pointer_x >= x + 7 && rfscan.pointer_x < x + popup_width - 7) {
                rfscan_edit_key(rfscan.pointer_x < x + popup_width / 2 ? SOLAR_OS_KEY_ENTER : SOLAR_OS_KEY_ESCAPE,
                    rfscan_popup_cells(gfx));
            }
            rfscan_render(ctx);
            return true;
        }
        if (rfscan.pointer_y >= height - 24) {
            rfscan.focus = rfscan.pointer_x * 5 / width;
            rfscan_action(rfscan.focus, 0);
        } else if (rfscan.pointer_y >= 18 && rfscan.pointer_y < 39) {
            rfscan_set_mode(rfscan.pointer_x >= width / 2);
        }
    } else return false;
    rfscan_render(ctx);
    return true;
}

static bool rfscan_release_ready(void)
{
    return rfscan_state == NULL || solar_os_radio_scan_worker_done(rfscan.worker);
}

static void rfscan_release_cleanup(void)
{
    if (rfscan_state != NULL) (void)solar_os_radio_scan_worker_close(&rfscan.worker, NULL);
}

const solar_os_app_t solar_os_rfscan_app = {
    .name = "rfscan",
    .summary = "radio energy graph and swept waterfall",
    .app_class = SOLAR_OS_APP_CLASS_GUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE | SOLAR_OS_APP_FLAG_POINTER_EVENTS,
    .start = rfscan_start,
    .stop = rfscan_close,
    .suspend = rfscan_suspend,
    .resume = rfscan_resume,
    .event = rfscan_event,
    .state_slot = &rfscan_state,
    .state_size = sizeof(rfscan_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .state_release_ready = rfscan_release_ready,
    .state_release_cleanup = rfscan_release_cleanup,
    .worker_stack_bytes = 4096U,
    /* Full-frame presentation on a streamed color display takes up to ~100 ms
     * on T-Deck. Radio sampling runs independently in the scan worker. */
    .tick_deadline_ms = 125U,
};
