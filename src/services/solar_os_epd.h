#pragma once

#include "drivers/epd_pixel_budget.h"

#define SOLAR_OS_EPD_REDRIVE_OPTIONS \
    "black-redrives=<0..31> white-redrives=<0..31> changed-drive-frames=<1..32> "

/* Policy and target accounting shared by EPD panel adapters. The adapter owns
 * storage, serialization, SPI, native baselines, waveform execution and power.
 * No allocation is required by native-only panels. */
typedef struct {
    epd_pulse_tuning_t tuning;
    epd_pixel_budget_t budget;
    uint32_t updates;
    int64_t last_frame_start;
    int64_t last_transfer_us, last_busy_us;
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
    esp_err_t (*allocate)(void *context);
    esp_err_t (*baseline)(void *context, const uint8_t *target);
    /* Complete one physical waveform, including BUSY/LUT wait and budget
     * advancement. An error must leave the budget unconsumed. */
    esp_err_t (*drive)(void *context);
    esp_err_t (*idle)(void *context);
} solar_os_epd_adapter_t;

bool solar_os_epd_cleanup_due(const solar_os_epd_t *state);
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
