#pragma once

#include "drivers/epd_pixel_budget.h"
#include "drivers/epd_scan_worker.h"

#define SOLAR_OS_EPD_REDRIVE_OPTIONS \
    "black-redrives=<0..31> white-redrives=<0..31> changed-drive-frames=<1..32> "
#define SOLAR_OS_EPD_MAINTENANCE_OPTIONS "cleanup-interval=<0..86400> "

typedef enum {
    SOLAR_OS_EPD_KEEP, SOLAR_OS_EPD_PARTIAL, SOLAR_OS_EPD_FULL,
} solar_os_epd_action_t;

typedef enum {
    SOLAR_OS_EPD_REASON_NONE, SOLAR_OS_EPD_REASON_BASELINE,
    SOLAR_OS_EPD_REASON_EXPLICIT, SOLAR_OS_EPD_REASON_ENCODING,
    SOLAR_OS_EPD_REASON_COUNT, SOLAR_OS_EPD_REASON_TIME,
    SOLAR_OS_EPD_REASON_EXPOSURE, SOLAR_OS_EPD_REASON_RECOVERY,
    SOLAR_OS_EPD_REASON_NATIVE,
} solar_os_epd_reason_t;

/* Small native policy state: no frame or per-pixel allocation. Interval zero
 * preserves the calibrated panel's update/exposure limit. A nonzero interval
 * selects timed routine cleanup, due on the next changed image. */
typedef struct {
    uint32_t interval_s, cleanups;
    int64_t last_cleanup_us;
    bool recorded;
    solar_os_epd_reason_t last_reason;
} solar_os_epd_maintenance_t;

typedef struct {
    solar_os_epd_action_t action;
    solar_os_epd_reason_t reason;
} solar_os_epd_plan_t;

solar_os_epd_plan_t solar_os_epd_native_plan(const solar_os_epd_maintenance_t *state,
    bool changed, bool baseline_valid, bool forced, bool encoding_changed, bool count_due);
bool solar_os_epd_time_due(const solar_os_epd_maintenance_t *state);
void solar_os_epd_note_cleanup(solar_os_epd_maintenance_t *state, solar_os_epd_reason_t reason);
esp_err_t solar_os_epd_set_maintenance(solar_os_epd_maintenance_t *state, const char *mode);
void solar_os_epd_append_maintenance(const solar_os_epd_maintenance_t *state, char *text, size_t size);

/* Policy and target accounting shared by EPD panel adapters. The adapter owns
 * storage, serialization, SPI, native baselines, waveform execution and power.
 * No allocation is required by native-only panels. */
typedef struct {
    epd_pulse_tuning_t tuning;
    epd_pixel_budget_t budget;
    uint32_t updates;
    int64_t last_frame_start;
    int64_t last_transfer_us, last_busy_us;
    solar_os_epd_maintenance_t maintenance;
} solar_os_epd_t;

typedef struct {
    solar_os_epd_t *state;
    void *context;
    uint8_t *shadow;
    size_t frame_bytes;
    unsigned width, height;
    /* Budget order can follow physical rows or U8g2 tile bytes. */
    bool tile_budget;
    bool *shadow_valid, *needs_cleanup, *mask_dirty, *window_valid;
    bool require_baseline, asynchronous, count_on_accept;
    unsigned period_ms;
    epd_scan_worker_t *worker;
    esp_err_t (*allocate)(void *context);
    esp_err_t (*baseline)(void *context, const uint8_t *target);
    /* Complete one physical waveform, including BUSY/LUT wait and budget
     * advancement. An error must leave the budget unconsumed. */
    esp_err_t (*drive)(void *context);
    esp_err_t (*idle)(void *context);
} solar_os_epd_adapter_t;

typedef struct {
    void *(*allocate)(size_t bytes, const char *tag);
    void (*release)(void *memory);
} solar_os_epd_storage_ops_t;

/* Allocate all pulse storage transactionally. Native paths never call this. */
esp_err_t solar_os_epd_allocate_budget(solar_os_epd_t *state, size_t pixels,
    uint8_t **planes, size_t plane_bytes, const solar_os_epd_storage_ops_t *storage);
void solar_os_epd_free_budget(solar_os_epd_t *state, uint8_t **planes,
    const solar_os_epd_storage_ops_t *storage);
/* Called after the adapter has disabled submissions and invalidated hardware
 * state. Restore the newest accepted snapshot using a native waveform. */
esp_err_t solar_os_epd_recover(const solar_os_epd_adapter_t *adapter);

bool solar_os_epd_cleanup_due(const solar_os_epd_t *state);
/* U8g2 vertical tile storage; unused bits in the final tile row are ignored. */
bool solar_os_epd_mono_changed(const uint8_t *target, const uint8_t *previous,
    unsigned width, unsigned height);
unsigned solar_os_epd_drive_limit(const solar_os_epd_t *state);
void solar_os_epd_append_mode(const solar_os_epd_t *state, char *text, size_t size);
void solar_os_epd_seed(solar_os_epd_t *state, const uint8_t *target,
                       unsigned width, unsigned height, bool tile_budget);
bool solar_os_epd_retarget(const solar_os_epd_adapter_t *adapter,
                           const uint8_t *target);
esp_err_t solar_os_epd_prepare(const solar_os_epd_adapter_t *adapter,
                              const uint8_t *target, bool fresh, bool *baseline);
esp_err_t solar_os_epd_step(const solar_os_epd_adapter_t *adapter,
                           const uint8_t *target, bool fresh,
                           bool *pending, bool *scanned);
esp_err_t solar_os_epd_flush(const solar_os_epd_adapter_t *adapter,
                            const uint8_t *target);
