#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* The callback owns panel locking, transport, and recovery. It performs at
 * most one drive cycle. new_target=false retains the preceding snapshot,
 * including on identical submissions that may require tuning/cleanup work.
 * pending keeps the worker running without requiring another presentation.
 */
typedef esp_err_t (*epd_scan_process_fn)(void *context, const uint8_t *target,
                                        bool new_target, bool *pending, bool *scanned);

typedef struct {
    uint32_t submitted, coalesced, frames, scans;
    int64_t last_scan_us, last_interval_us, last_frame_interval_us;
} epd_scan_stats_t;

typedef struct {
    TaskHandle_t task;
    SemaphoreHandle_t lock, done;
    StaticSemaphore_t lock_storage, done_storage;
    uint8_t *latest, *snapshot;
    size_t frame_size;
    epd_scan_process_fn process;
    void *context;
    uint16_t period_ms;
    bool enabled, stopping, has_target;
    uint32_t generation;
    epd_scan_stats_t stats;
} epd_scan_worker_t;

esp_err_t epd_scan_worker_init(epd_scan_worker_t *worker, size_t frame_size,
                               epd_scan_process_fn process, void *context);
/* Disabling rejects submissions and discards queued targets. An in-flight
 * callback finishes; the adapter must serialize panel mode/power changes.
 */
void epd_scan_worker_enable(epd_scan_worker_t *worker, bool enabled);
esp_err_t epd_scan_worker_submit(epd_scan_worker_t *worker, const uint8_t *frame);
/* Minimum interval between distinct target snapshots. Pending settling cycles
 * run as fast as the panel callback permits and do not consume this interval.
 */
esp_err_t epd_scan_worker_set_period(epd_scan_worker_t *worker, unsigned period_ms);
epd_scan_stats_t epd_scan_worker_stats(epd_scan_worker_t *worker);
/* Join before freeing panel resources. Do not call from inside the callback
 * or while holding a lock needed by the callback.
 */
void epd_scan_worker_deinit(epd_scan_worker_t *worker);
