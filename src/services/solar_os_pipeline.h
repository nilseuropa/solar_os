#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SOLAR_OS_PIPELINES_MAX 4U
typedef struct {
    /* Absolute file path, native video stream ID, or rtsp:// JPEG URL. */
    const char *source;
    const char *processor; /* qr or model */
    uint32_t model, limit, interval_ms, timeout_ms;
} solar_os_pipeline_config_t;
typedef struct {
    uint32_t id, model;
    char job[20], state[12], source[256], processor[12];
    uint64_t sequence, frames, busy_frames, source_timestamp_us, decode_us, process_us, frame_us;
    esp_err_t last_error;
    bool done;
} solar_os_pipeline_status_t;

/* Native jobs outlive clients. Up to four configurations/latest-result slots
 * remain until explicit destroy. Stop joins and releases sources/processors;
 * retained models remain resident. Handles never recycle during a boot. */
esp_err_t solar_os_pipeline_start(const solar_os_pipeline_config_t *config, uint32_t *id);
esp_err_t solar_os_pipeline_list(uint32_t *ids, size_t capacity, size_t *count);
esp_err_t solar_os_pipeline_status(uint32_t id, solar_os_pipeline_status_t *status);
/* Non-destructive snapshot. NULL means no new result after `after`. The caller
 * owns the copied JSON (solar_os_memory_free). Sequence and frame timing are
 * included in each result to keep them consistent with that frame. */
esp_err_t solar_os_pipeline_result(uint32_t id, uint64_t after, char **json);
esp_err_t solar_os_pipeline_stop(uint32_t id);
esp_err_t solar_os_pipeline_destroy(uint32_t id);
