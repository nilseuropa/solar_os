#include "solar_os_radio_scan.h"

#include <string.h>

#include "esp_timer.h"

#define SCAN_SETTLE_MS 20U

static uint32_t scan_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void solar_os_radio_scan_clear(solar_os_radio_scan_t *scan)
{
    if (scan == NULL) return;
    for (size_t i = 0; i < SOLAR_OS_RADIO_SCAN_BINS; i++) {
        scan->levels[i] = SOLAR_OS_RADIO_SCAN_EMPTY;
        scan->peaks[i] = SOLAR_OS_RADIO_SCAN_EMPTY;
    }
    memset(scan->waterfall, INT8_MIN, sizeof(scan->waterfall));
    scan->history_count = scan->history_head = scan->rows = 0;
    if (scan->config.mode == SOLAR_OS_RADIO_SCAN_BAND && scan->running) {
        scan->bin = 0;
        scan->settling = false;
    }
    scan->sweeps = 0;
    scan->last_sweep_ms = 0;
    scan->sweep_started_ms = scan_now();
    scan->latest = scan->floor = scan->peak = SOLAR_OS_RADIO_SCAN_EMPTY;
    scan->recent_peak = SOLAR_OS_RADIO_SCAN_EMPTY;
    scan->recent_peak_frequency_hz = 0;
    scan->generation++;
}

void solar_os_radio_scan_init(solar_os_radio_scan_t *scan)
{
    if (scan == NULL) return;
    memset(scan, 0, sizeof(*scan));
    scan->handle = (solar_os_radio_handle_t)SOLAR_OS_RADIO_HANDLE_INIT;
    solar_os_radio_scan_clear(scan);
}

esp_err_t solar_os_radio_scan_stop(solar_os_radio_scan_t *scan)
{
    if (scan == NULL) return ESP_ERR_INVALID_ARG;
    scan->running = false;
    scan->settling = false;
    esp_err_t err = ESP_OK;
    if (scan->restore) {
        err = solar_os_radio_handle_configure(&scan->handle, &scan->saved.config);
        if (err == ESP_OK) {
            err = solar_os_radio_handle_set_state(&scan->handle, scan->saved.state);
        } else {
            /* A failed restore must not leave a partially configured RX/TX. */
            (void)solar_os_radio_handle_set_state(&scan->handle, SOLAR_OS_RADIO_STATE_STANDBY);
        }
        scan->restore = false;
    }
    if (scan->handle.index >= 0) {
        esp_err_t released = solar_os_radio_release(&scan->handle);
        if (err == ESP_OK) err = released;
    }
    scan->error = err;
    scan->generation++;
    return err;
}

esp_err_t solar_os_radio_scan_start(solar_os_radio_scan_t *scan, const char *name,
                                   const solar_os_radio_scan_config_t *config)
{
    if (scan == NULL || name == NULL || config == NULL ||
        (config->mode != SOLAR_OS_RADIO_SCAN_CHANNEL && config->mode != SOLAR_OS_RADIO_SCAN_BAND)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (scan->running || scan->handle.index >= 0) return ESP_ERR_INVALID_STATE;
    if (config->mode == SOLAR_OS_RADIO_SCAN_BAND &&
        (config->step_hz == 0 || config->start_hz == 0 || config->end_hz < config->start_hz ||
         (config->end_hz - config->start_hz) / config->step_hz >= SOLAR_OS_RADIO_SCAN_BINS)) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_radio_info_t info;
    esp_err_t err = solar_os_radio_get_info(name, &info);
    if (err != ESP_OK) return err;
    if (!(info.features & SOLAR_OS_RADIO_FEATURE_LIVE_RSSI)) return ESP_ERR_NOT_SUPPORTED;
    scan->fast_tune = (info.features & SOLAR_OS_RADIO_FEATURE_FAST_TUNE) != 0;
    scan->settle_ms = SCAN_SETTLE_MS;
    strlcpy(scan->name, name, sizeof(scan->name));
    strlcpy(scan->owner, info.owner, sizeof(scan->owner));
    scan->config = *config;
    if (scan->config.interval_ms < 100U) scan->config.interval_ms = 100U;
    scan->error = ESP_OK;
    solar_os_radio_scan_clear(scan);
    scan->bins = config->mode == SOLAR_OS_RADIO_SCAN_BAND ?
        (config->end_hz - config->start_hz) / config->step_hz + 1U : 0;
    scan->bin = 0;
    err = solar_os_radio_get_status(name, &scan->saved);
    if (err != ESP_OK) return err;
    /* Only observing an already receiving channel is shared. All retuning or
     * enabling RX requires an exclusive claim; never steal an owner's radio. */
    bool channel_retune = config->mode == SOLAR_OS_RADIO_SCAN_CHANNEL &&
        ((config->frequency_hz && config->frequency_hz != scan->saved.config.frequency_hz) ||
         (config->bandwidth_hz && config->bandwidth_hz != scan->saved.config.rx_bandwidth_hz));
    if (config->mode == SOLAR_OS_RADIO_SCAN_BAND || channel_retune ||
        scan->saved.state != SOLAR_OS_RADIO_STATE_RX) {
        err = solar_os_radio_claim(name, "rfscan", &scan->handle);
        if (err != ESP_OK) {
            if (solar_os_radio_get_info(name, &info) == ESP_OK) {
                strlcpy(scan->owner, info.owner, sizeof(scan->owner));
            }
            return err;
        }
        err = solar_os_radio_handle_get_status(&scan->handle, &scan->saved);
        if (err == ESP_OK && (scan->saved.state == SOLAR_OS_RADIO_STATE_TX ||
                              scan->saved.state == SOLAR_OS_RADIO_STATE_UNKNOWN)) {
            err = ESP_ERR_INVALID_STATE;
        }
        if (err != ESP_OK) {
            (void)solar_os_radio_scan_stop(scan);
            return err;
        }
        scan->restore = true;
    }
    scan->receiver = scan->saved.config;
    if (config->mode == SOLAR_OS_RADIO_SCAN_BAND) {
        if (config->bandwidth_hz) scan->receiver.rx_bandwidth_hz = config->bandwidth_hz;
        /* Fast drivers initialize the modem once, then validate both endpoints
         * through their tuning operation. Older drivers validate by configure. */
        scan->receiver.frequency_hz = scan->fast_tune ? config->start_hz : config->end_hz;
        err = solar_os_radio_handle_configure(&scan->handle, &scan->receiver);
        scan->receiver.frequency_hz = config->start_hz;
        if (err == ESP_OK && scan->fast_tune) {
            err = solar_os_radio_handle_tune_rx(&scan->handle, config->end_hz, &scan->settle_ms);
            if (err == ESP_OK) err = solar_os_radio_handle_tune_rx(
                &scan->handle, config->start_hz, &scan->settle_ms);
        } else if (err == ESP_OK) {
            err = solar_os_radio_handle_configure(&scan->handle, &scan->receiver);
        }
        if (err == ESP_OK) {
            solar_os_radio_status_t effective;
            err = solar_os_radio_handle_get_status(&scan->handle, &effective);
            if (err == ESP_OK) scan->receiver = effective.config;
        }
        if (err == ESP_OK && !scan->fast_tune) {
            err = solar_os_radio_handle_set_state(&scan->handle, SOLAR_OS_RADIO_STATE_RX);
        }
        scan->settling = true;
    } else if (scan->restore) {
        if (config->frequency_hz) scan->receiver.frequency_hz = config->frequency_hz;
        if (config->bandwidth_hz) scan->receiver.rx_bandwidth_hz = config->bandwidth_hz;
        if (scan->receiver.frequency_hz != scan->saved.config.frequency_hz ||
            scan->receiver.rx_bandwidth_hz != scan->saved.config.rx_bandwidth_hz) {
            err = solar_os_radio_handle_configure(&scan->handle, &scan->receiver);
        }
        if (err == ESP_OK) {
            solar_os_radio_status_t effective;
            err = solar_os_radio_handle_get_status(&scan->handle, &effective);
            if (err == ESP_OK) scan->receiver = effective.config;
        }
        if (err == ESP_OK) err = solar_os_radio_handle_set_state(&scan->handle, SOLAR_OS_RADIO_STATE_RX);
    }
    if (err != ESP_OK) {
        (void)solar_os_radio_scan_stop(scan);
        scan->error = err;
        return err;
    }
    scan->sweep_started_ms = scan_now();
    scan->next_us = esp_timer_get_time() + (int64_t)scan->settle_ms * 1000;
    scan->running = true;
    return ESP_OK;
}

esp_err_t solar_os_radio_scan_poll(solar_os_radio_scan_t *scan)
{
    if (scan == NULL) return ESP_ERR_INVALID_ARG;
    if (!scan->running) return ESP_OK;
    uint32_t now = scan_now();
    if (esp_timer_get_time() < scan->next_us) return ESP_OK;
    esp_err_t err;
    if (scan->config.mode == SOLAR_OS_RADIO_SCAN_BAND && !scan->settling) {
        scan->receiver.frequency_hz = scan->config.start_hz + scan->bin * scan->config.step_hz;
        if (scan->fast_tune) {
            err = solar_os_radio_handle_tune_rx(&scan->handle, scan->receiver.frequency_hz, &scan->settle_ms);
        } else {
            err = solar_os_radio_handle_configure(&scan->handle, &scan->receiver);
            if (err == ESP_OK) err = solar_os_radio_handle_set_state(&scan->handle, SOLAR_OS_RADIO_STATE_RX);
        }
        if (err == ESP_OK) {
            scan->settling = true;
            scan->next_us = esp_timer_get_time() + (int64_t)scan->settle_ms * 1000;
            return ESP_OK;
        }
    } else {
        if (scan->config.mode == SOLAR_OS_RADIO_SCAN_CHANNEL) {
            solar_os_radio_status_t status;
            err = solar_os_radio_get_status(scan->name, &status);
            if (err != ESP_OK) goto failed;
            if (status.config.frequency_hz != scan->receiver.frequency_hz ||
                status.config.rx_bandwidth_hz != scan->receiver.rx_bandwidth_hz) {
                solar_os_radio_scan_clear(scan);
            }
            scan->receiver = status.config;
        }
        int16_t dbm;
        err = solar_os_radio_read_rssi(scan->name, &dbm);
        if (err == ESP_ERR_INVALID_STATE && scan->config.mode == SOLAR_OS_RADIO_SCAN_CHANNEL &&
            !scan->restore) {
            /* The owner can be transmitting or asleep: do not plot stale RSSI. */
            scan->next_us = esp_timer_get_time() + (int64_t)scan->config.interval_ms * 1000;
            return ESP_OK;
        }
        if (err == ESP_OK) {
            scan->latest = dbm;
            if (scan->recent_peak == SOLAR_OS_RADIO_SCAN_EMPTY || dbm > scan->recent_peak) {
                scan->recent_peak = dbm;
                scan->recent_peak_frequency_hz = scan->receiver.frequency_hz;
            }
            if (scan->floor == SOLAR_OS_RADIO_SCAN_EMPTY || dbm < scan->floor) scan->floor = dbm;
            if (scan->peak == SOLAR_OS_RADIO_SCAN_EMPTY || dbm > scan->peak) scan->peak = dbm;
            now = scan_now();
            if (scan->config.mode == SOLAR_OS_RADIO_SCAN_CHANNEL) {
                scan->history[scan->history_head] = dbm;
                scan->history_ms[scan->history_head] = now;
                scan->history_head = (scan->history_head + 1U) % SOLAR_OS_RADIO_SCAN_HISTORY;
                if (scan->history_count < SOLAR_OS_RADIO_SCAN_HISTORY) scan->history_count++;
                scan->next_us = esp_timer_get_time() + (int64_t)scan->config.interval_ms * 1000;
            } else {
                scan->levels[scan->bin] = dbm;
                if (scan->peaks[scan->bin] == SOLAR_OS_RADIO_SCAN_EMPTY || dbm > scan->peaks[scan->bin]) {
                    scan->peaks[scan->bin] = dbm;
                }
                scan->bin++;
                scan->settling = false;
                if (scan->bin == scan->bins) {
                    memmove(scan->waterfall[1], scan->waterfall[0],
                            (SOLAR_OS_RADIO_SCAN_ROWS - 1U) * sizeof(scan->waterfall[0]));
                    for (size_t i = 0; i < scan->bins; i++) {
                        int level = scan->levels[i];
                        scan->waterfall[0][i] = level < -127 ? -127 : level > 0 ? 0 : level;
                    }
                    scan->bin = 0;
                    if (scan->rows < SOLAR_OS_RADIO_SCAN_ROWS) scan->rows++;
                    scan->sweeps++;
                    scan->last_sweep_ms = now - scan->sweep_started_ms;
                    scan->sweep_started_ms = now;
                }
                scan->next_us = esp_timer_get_time();
            }
            scan->generation++;
            return ESP_OK;
        }
    }
failed:
    (void)solar_os_radio_scan_stop(scan);
    scan->error = err;
    return err;
}
