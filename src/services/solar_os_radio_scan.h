#pragma once

#include "solar_os_radio.h"

#define SOLAR_OS_RADIO_SCAN_BINS 129U
#define SOLAR_OS_RADIO_SCAN_ROWS 48U
#define SOLAR_OS_RADIO_SCAN_HISTORY 256U
#define SOLAR_OS_RADIO_SCAN_EMPTY INT16_MIN

typedef enum {
    SOLAR_OS_RADIO_SCAN_CHANNEL,
    SOLAR_OS_RADIO_SCAN_BAND,
} solar_os_radio_scan_mode_t;

typedef struct {
    solar_os_radio_scan_mode_t mode;
    uint32_t start_hz;
    uint32_t end_hz;
    uint32_t step_hz;
    uint32_t frequency_hz; /* Channel: zero follows the current receiver. */
    uint32_t bandwidth_hz; /* Zero keeps the current receiver bandwidth. */
    uint32_t interval_ms;  /* Channel sampling interval, at least 100 ms. */
} solar_os_radio_scan_config_t;

/* The core context is caller-serialized. It can be driven synchronously or by
 * the optional worker below; graphical consumers read independent copies. */
typedef struct {
    solar_os_radio_scan_config_t config;
    solar_os_radio_status_t saved;
    solar_os_radio_config_t receiver;
    solar_os_radio_handle_t handle;
    char name[SOLAR_OS_RADIO_NAME_MAX];
    char owner[SOLAR_OS_RADIO_OWNER_MAX];
    bool running;
    bool restore;
    bool settling;
    bool fast_tune;
    uint32_t settle_ms;
    int64_t next_us;
    uint32_t generation;
    uint32_t sweeps;
    uint32_t sweep_started_ms;
    uint32_t last_sweep_ms;
    size_t bins;
    size_t bin;
    size_t rows;
    size_t history_count;
    size_t history_head;
    int16_t latest;
    int16_t recent_peak; /* Strongest fresh sample since the last worker copy. */
    uint32_t recent_peak_frequency_hz;
    int16_t floor;
    int16_t peak;
    int16_t levels[SOLAR_OS_RADIO_SCAN_BINS];
    int16_t peaks[SOLAR_OS_RADIO_SCAN_BINS];
    int8_t waterfall[SOLAR_OS_RADIO_SCAN_ROWS][SOLAR_OS_RADIO_SCAN_BINS];
    int16_t history[SOLAR_OS_RADIO_SCAN_HISTORY];
    uint32_t history_ms[SOLAR_OS_RADIO_SCAN_HISTORY];
    esp_err_t error;
} solar_os_radio_scan_t;

void solar_os_radio_scan_init(solar_os_radio_scan_t *scan);
esp_err_t solar_os_radio_scan_start(solar_os_radio_scan_t *scan, const char *name,
                                   const solar_os_radio_scan_config_t *config);
/* Performs at most one tune or sample; settling never blocks the caller. */
esp_err_t solar_os_radio_scan_poll(solar_os_radio_scan_t *scan);
/* Always releases an acquired radio, reporting any restoration failure. */
esp_err_t solar_os_radio_scan_stop(solar_os_radio_scan_t *scan);
void solar_os_radio_scan_clear(solar_os_radio_scan_t *scan);

/* Optional service-owned sampler. UI code reads copies and never holds a
 * sampling lock while drawing. Close joins before releasing the radio/state. */
typedef struct solar_os_radio_scan_worker solar_os_radio_scan_worker_t;
esp_err_t solar_os_radio_scan_worker_open(solar_os_radio_scan_worker_t **worker,
    const char *name, const solar_os_radio_scan_config_t *config);
/* Copy a snapshot and consume its recent_peak interval. EMPTY means no fresh
 * samples; latest and the held peaks remain available for display. */
esp_err_t solar_os_radio_scan_worker_copy(solar_os_radio_scan_worker_t *worker,
    solar_os_radio_scan_t *snapshot);
esp_err_t solar_os_radio_scan_worker_clear(solar_os_radio_scan_worker_t *worker);
bool solar_os_radio_scan_worker_done(const solar_os_radio_scan_worker_t *worker);
/* On timeout keep *worker alive; retry close after worker_done becomes true. */
esp_err_t solar_os_radio_scan_worker_close(solar_os_radio_scan_worker_t **worker,
    solar_os_radio_scan_t *snapshot);
